#include "core/jam_mode.h"

#include "audio/audio_engine.h"
#include "yandex/library.h"

#include <QHash>
#include <QPointer>
#include <QtGlobal>

#include <algorithm>
#include <functional>
#include <utility>

namespace Core {

using Yandex::Track;

// The jam wave's session and, for every wave track, the session, station and batch it came in:
// a track's feedback goes to its own session even after a new one started (HOST-11, TRK-08).
struct JamMode::Wave {
    struct Origin {
        QString session;
        QString station;
        QString batch;
    };

    bool feedback = false;
    QString session;
    QString station;
    int seedsVersion = -1;
    QHash<QString, Origin> originOfTrack;

    void addBatch(const Yandex::WaveBatch& batch) {
        for (const Track& track : batch.tracks) {
            originOfTrack.insert(track.id, Origin{session, station, batch.batchId});
        }
    }
};

namespace {

Yandex::WaveEvent WaveEventOf(Player::TrackEvent event) {
    switch (event) {
        case Player::TrackEvent::Started: return Yandex::WaveEvent::TrackStarted;
        case Player::TrackEvent::Finished: return Yandex::WaveEvent::TrackFinished;
        case Player::TrackEvent::Skipped: break;
    }
    return Yandex::WaveEvent::Skip;
}

QList<Track> Playable(const QList<Track>& tracks) {
    QList<Track> playable;
    std::copy_if(
        tracks.cbegin(), tracks.cend(), std::back_inserter(playable),
        [](const Track& track) { return track.available; }
    );
    return playable;
}

}  // namespace

JamMode::JamMode(Player* player, Yandex::Library* library, QObject* parent)
    : QObject(parent)
    , corePlayer(player)
    , yandexLibrary(library)
    , wave(std::make_shared<Wave>()) {
    reportTimer.setInterval(kJamPlayingReportMs);
    connect(&reportTimer, &QTimer::timeout, this, [this] {
        if (corePlayer->engine()->state() == Audio::AudioEngine::State::Playing) {
            reportPlayback();
        }
    });
    connect(corePlayer, &Player::currentTrackChanged, this, [this] {
        if (!active) {
            return;
        }
        schedulePlaybackReport();
        QTimer::singleShot(0, this, &JamMode::dropDeferredWave);
    });
    connect(corePlayer, &Player::seeked, this, &JamMode::schedulePlaybackReport);
    connect(
        corePlayer->engine(), &Audio::AudioEngine::stateChanged, this,
        &JamMode::schedulePlaybackReport
    );
    connect(corePlayer, &Player::playlistChanged, this, &JamMode::checkSlots);
}

JamMode::~JamMode() = default;

JamSlot JamMode::currentSlot() const {
    const int cursor = corePlayer->currentIndex();
    return cursor >= 0 && cursor < trackSlots.size() ? trackSlots[cursor] : JamSlot{};
}

void JamMode::start(const QString& title, bool waveFeedback) {
    if (active) {
        return;
    }
    wave = std::make_shared<Wave>();
    wave->feedback = waveFeedback;
    lastSeeds.clear();
    lastSeedsVersion = -1;

    const int cursor = corePlayer->currentIndex();
    QList<int> others;
    for (int i = 0; i < corePlayer->playlist().size(); ++i) {
        if (i != cursor) {
            others << i;
        }
    }
    if (!others.isEmpty()) {
        corePlayer->removeTracks(others);  // HOST-14: the playing track stays, the rest goes
    }
    trackSlots = corePlayer->playlist().isEmpty() ? QList<JamSlot>{} : QList<JamSlot>{JamSlot{}};
    active = true;

    QPointer<JamMode> self(this);
    Yandex::Library* library = yandexLibrary;
    const std::shared_ptr<Wave> state = wave;
    Player::TLoadMoreCallback more = [self](std::function<void(const QList<Track>&)> done) {
        if (!self || !self->active) {
            return done({});
        }
        self->loadWave(std::move(done));
    };
    Player::TEventCallback events = [self, library, state](
                                        Player::TrackEvent event, const Track& track, double played
                                    ) {
        if (self && self->active) {
            const JamSlot slot = self->currentSlot();
            if (event == Player::TrackEvent::Started && slot.kind == JamSlot::Kind::Item) {
                Q_EMIT self->itemStarted(slot.itemId);  // HOST-05
            }
            self->schedulePlaybackReport();
        }
        const auto origin = state->originOfTrack.constFind(track.id);
        const bool waveTrack = origin != state->originOfTrack.constEnd()
            && (!self || !self->active || self->currentSlot().kind == JamSlot::Kind::Wave);
        if (waveTrack && state->feedback) {  // HOST-16: the jam wave's own session only
            library->waveFeedback(
                origin->session, origin->station, origin->batch, WaveEventOf(event), &track, played
            );
        }
    };
    // HOST-15: no play reports; HOST-07: Previous restarts the track.
    corePlayer->changeSource(title, std::move(more), std::move(events), QueueRules{false, true});
    reportTimer.start();
    reportPlayback();
}

void JamMode::setQueue(const QList<JamEntry>& entries, const QStringList& seeds, int seedsVersion) {
    if (!active) {
        return;
    }
    lastSeeds = seeds;
    lastSeedsVersion = seedsVersion;
    if (corePlayer->playlist().isEmpty()) {
        if (!entries.isEmpty()) {  // HOST-08: nothing plays, the first item starts at once
            insertItems(0, entries);
            corePlayer->playIndex(0);
        }
        return;
    }
    const int cursor = corePlayer->currentIndex();
    const JamSlot current = currentSlot();
    const bool tailWasEmpty = cursor + 1 >= corePlayer->playlist().size();
    QList<JamEntry> wanted;
    for (const JamEntry& entry : entries) {
        if (current.kind != JamSlot::Kind::Item || entry.itemId != current.itemId) {
            wanted << entry;  // HOST-04: the current item never goes back into the tail
        }
    }
    int end = cursor + 1;
    while (end < trackSlots.size() && trackSlots[end].kind == JamSlot::Kind::Item) {
        ++end;
    }
    int prefix = 0;
    while (cursor + 1 + prefix < end && prefix < wanted.size()
           && trackSlots[cursor + 1 + prefix].itemId == wanted[prefix].itemId) {
        ++prefix;  // HOST-01, HOST-02: the common prefix stays, with its preload
    }
    QList<int> changed;
    for (int i = cursor + 1 + prefix; i < end; ++i) {
        changed << i;
    }
    if (!changed.isEmpty()) {
        removeSlots(changed);
    }
    const QList<JamEntry> added = wanted.mid(prefix);
    if (!added.isEmpty()) {
        insertItems(cursor + 1 + prefix, added);
        if (corePlayer->isWaitingForMore() || (tailWasEmpty && !corePlayer->trackInProgress())) {
            corePlayer->playIndex(cursor + 1);  // HOST-08: nothing plays, the item starts at once
        }
    }
    dropDeferredWave();
    corePlayer->requestMore();
}

void JamMode::skip(const QString& itemId) {
    const JamSlot slot = currentSlot();
    if (active && slot.kind == JamSlot::Kind::Item && slot.itemId == itemId) {
        corePlayer->next();  // HOST-18; any other item: HOST-19
    }
}

void JamMode::end() {
    if (!active) {
        return;
    }
    reportTimer.stop();
    const int cursor = corePlayer->currentIndex();
    QList<int> waveTracks;
    for (int i = cursor + 1; i < trackSlots.size(); ++i) {
        if (trackSlots[i].kind == JamSlot::Kind::Wave) {
            waveTracks << i;
        }
    }
    active = false;
    trackSlots.clear();
    if (!waveTracks.isEmpty()) {
        corePlayer->removeTracks(waveTracks);  // HOST-32: the jam items stay as ordinary tracks
    }
    corePlayer->changeSource(corePlayer->queueTitle(), {}, {}, QueueRules{});  // HOST-33
}

void JamMode::reportPlayback() {
    if (!active) {
        return;
    }
    JamPlayback report;
    const Track* track = corePlayer->currentTrack();
    const auto state = corePlayer->engine()->state();
    const bool stopped = state == Audio::AudioEngine::State::Stopped;
    if (track && !(stopped && corePlayer->isWaitingForMore())) {
        const JamSlot slot = currentSlot();
        report.kind =
            slot.kind == JamSlot::Kind::Item ? JamPlayback::Kind::Item : JamPlayback::Kind::Wave;
        report.itemId = slot.kind == JamSlot::Kind::Item ? slot.itemId : QString();
        report.track = *track;
        report.link = corePlayer->currentLink();
        report.nextLink = corePlayer->nextLink();
        report.positionMs =
            stopped ? 0 : static_cast<qint64>(corePlayer->engine()->positionSeconds() * 1000);
        report.paused = state != Audio::AudioEngine::State::Playing;
    }
    Q_EMIT playback(report);
}

void JamMode::loadWave(std::function<void(const QList<Track>&)> done) {
    const std::shared_ptr<Wave> state = wave;
    QPointer<JamMode> self(this);
    if (!state->session.isEmpty() && state->seedsVersion == lastSeedsVersion) {  // HOST-11
        QStringList recent;
        for (qsizetype i = trackSlots.size() - 1; i >= 0 && recent.size() < kJamWaveHistory; --i) {
            if (trackSlots[i].kind == JamSlot::Kind::Wave && i < corePlayer->playlist().size()) {
                recent.prepend(corePlayer->playlist()[i].id);
            }
        }
        yandexLibrary->moreWave(
            state->session, recent,
            [self, state, done](const Yandex::WaveBatch& batch, const QString& error) {
                if (!error.isEmpty()) {
                    qWarning("jam wave: %s", qPrintable(error));
                }
                state->addBatch(batch);
                if (!self || !self->active) {
                    return done({});
                }
                self->appendWave(batch.tracks, done);
            }
        );
        return;
    }
    QStringList seeds = lastSeeds;
    if (const Track* current = corePlayer->currentTrack(); seeds.isEmpty() && current) {
        seeds = {QStringLiteral("track:") + current->id};  // HOST-13
    }
    if (seeds.isEmpty()) {  // HOST-13: nothing has played and there are no seeds
        done({});
        return reportPlayback();
    }
    const int seedsVersion = lastSeedsVersion;
    Yandex::Library* library = yandexLibrary;
    library->startWave(
        seeds,
        [self, state, done, seeds, seedsVersion,
         library](const Yandex::WaveBatch& batch, const QString& error) {
            if (!error.isEmpty() || Playable(batch.tracks).isEmpty()) {
                if (!error.isEmpty()) {
                    qWarning("jam wave: %s", qPrintable(error));
                }
                return done({});
            }
            state->session = batch.sessionId;
            state->station = seeds.value(0);
            state->seedsVersion = seedsVersion;
            state->addBatch(batch);
            if (state->feedback) {
                library->waveFeedback(
                    state->session, state->station, batch.batchId, Yandex::WaveEvent::RadioStarted
                );
            }
            if (!self || !self->active) {
                return done({});
            }
            self->appendWave(batch.tracks, done);
        }
    );
}

void JamMode::appendWave(
    const QList<Track>& tracks,
    std::function<void(const QList<Track>&)> done
) {
    const QList<Track> playable = Playable(tracks);
    for (qsizetype i = 0; i < playable.size(); ++i) {
        trackSlots << JamSlot{JamSlot::Kind::Wave, {}, {}};
    }
    done(playable);
    checkSlots();
}

void JamMode::dropDeferredWave() {
    if (!active || wave->session.isEmpty() || wave->seedsVersion == lastSeedsVersion
        || currentSlot().kind == JamSlot::Kind::Wave) {
        return;
    }
    const int cursor = corePlayer->currentIndex();
    QList<int> deferred;
    for (int i = cursor + 1; i < trackSlots.size(); ++i) {
        if (trackSlots[i].kind == JamSlot::Kind::Item) {
            return;  // the jam part is not empty yet
        }
        if (trackSlots[i].kind == JamSlot::Kind::Wave) {
            deferred << i;
        }
    }
    wave->session.clear();  // HOST-11: new seeds, a new session next
    if (!deferred.isEmpty()) {
        removeSlots(deferred);
    }
    corePlayer->requestMore();
}

void JamMode::removeSlots(const QList<int>& indices) {
    QList<int> sorted = indices;
    std::sort(sorted.begin(), sorted.end(), std::greater<>());
    for (int index : sorted) {
        trackSlots.removeAt(index);
    }
    corePlayer->removeTracks(indices);
}

void JamMode::insertItems(int at, const QList<JamEntry>& entries) {
    QList<Track> tracks;
    for (qsizetype i = 0; i < entries.size(); ++i) {
        trackSlots.insert(
            at + i, JamSlot{JamSlot::Kind::Item, entries[i].itemId, entries[i].addedBy}
        );
        Track track = entries[i].track;
        track.available = true;
        tracks << track;
    }
    corePlayer->insertTracks(at, tracks);
}

void JamMode::checkSlots() {
    if (!active || trackSlots.size() == corePlayer->playlist().size()) {
        return;
    }
    qWarning(
        "jam: the queue has %lld tracks but the jam knows %lld; the queue was changed outside it",
        static_cast<long long>(corePlayer->playlist().size()),
        static_cast<long long>(trackSlots.size())
    );
    trackSlots.resize(corePlayer->playlist().size());
}

void JamMode::schedulePlaybackReport() {
    if (!active || reportPending) {
        return;
    }
    reportPending = true;
    QTimer::singleShot(0, this, [this] {
        reportPending = false;
        reportPlayback();
    });
}

}  // namespace Core
