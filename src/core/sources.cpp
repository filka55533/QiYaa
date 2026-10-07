#include "core/sources.h"

#include "core/jam_mode.h"
#include "core/player.h"
#include "yandex/api_client.h"
#include "yandex/library.h"

#include <QCoreApplication>
#include <QHash>
#include <QList>
#include <QPointer>

#include <algorithm>
#include <functional>
#include <memory>

namespace Core {

using Yandex::Track;

namespace {

constexpr qsizetype kWaveHistory = 5;

// The tracks the Player would queue: the empty check of a source runs on these (SRC-08).
bool HasAvailable(const QList<Track>& tracks) {
    return std::any_of(tracks.cbegin(), tracks.cend(), [](const Track& track) {
        return track.available;
    });
}

void ShowStatus(Player* player, const QString& text) {
    Q_EMIT player->statusMessage(text);
}

// The callback for a request that loads a source's tracks: applies them if `ticket` is still the
// latest pick.
auto QueueLoader(Player* player, const QString& title, quint64 ticket) {
    QPointer<Player> guardedPlayer(player);
    return [guardedPlayer, title, ticket](const QList<Track>& tracks, const QString& error) {
        if (!guardedPlayer || !guardedPlayer->isLatestSourceRequest(ticket)) {
            return;
        }
        if (!error.isEmpty()) {
            return ShowStatus(
                guardedPlayer, QCoreApplication::translate("Core::Sources", "Error: %1").arg(error)
            );
        }
        if (!HasAvailable(tracks)) {  // SRC-07, SRC-08: the queue stays
            return ShowStatus(
                guardedPlayer, QCoreApplication::translate("Core::Sources", "%1: empty").arg(title)
            );
        }
        guardedPlayer->setQueue(tracks, title, true);
    };
}

// What a wave remembers for its feedback: the session, the station (the first seed) and which
// batch each track came in.
struct WaveState {
    QString session;
    QString station;
    QHash<QString, QString> batchOfTrack;
};

// The wave's callbacks hold the Library and the Player, not the Sources: the Player closes the
// open track, and sends its feedback, while the application shuts down.
Player::TLoadMoreCallback
WaveMore(QPointer<Player> player, Yandex::Library* library, std::shared_ptr<WaveState> state) {
    return [player, library, state](std::function<void(const QList<Track>&)> done) {
        if (!player) {
            return;
        }
        QStringList queue;
        const QList<Track>& playlist = player->playlist();
        for (qsizetype i = std::max<qsizetype>(0, playlist.size() - kWaveHistory);
             i < playlist.size(); ++i) {
            queue << playlist[i].id;
        }
        library->moreWave(
            state->session, queue,
            [done, state](const Yandex::WaveBatch& nextBatch, const QString& waveError) {
                if (!waveError.isEmpty()) {
                    qWarning("wave: %s", qPrintable(waveError));
                }
                for (const Track& track : nextBatch.tracks) {
                    state->batchOfTrack.insert(track.id, nextBatch.batchId);
                }
                done(nextBatch.tracks);
            }
        );
    };
}

Player::TEventCallback WaveEvents(Yandex::Library* library, std::shared_ptr<WaveState> state) {
    return [library, state](Player::TrackEvent event, const Track& track, double played) {
        const Yandex::WaveEvent waveEvent = event == Player::TrackEvent::Started
            ? Yandex::WaveEvent::TrackStarted
            : event == Player::TrackEvent::Finished ? Yandex::WaveEvent::TrackFinished
                                                    : Yandex::WaveEvent::Skip;
        library->waveFeedback(
            state->session, state->station, state->batchOfTrack.value(track.id), waveEvent, &track,
            played
        );
    };
}

}  // namespace

Sources::Sources(Player* player, Yandex::Library* library, QObject* parent)
    : QObject(parent)
    , corePlayer(player)
    , yandexLibrary(library) { }

void Sources::showStatus(const QString& text) {
    ShowStatus(corePlayer, text);
}

bool Sources::refusedForJam() {
    if (!jamMode || !jamMode->isActive()) {
        return false;
    }
    showStatus(tr("A jam is on: add tracks to the jam"));
    return true;
}

void Sources::playLikes(bool autoplay) {
    if (refusedForJam()) {
        return;
    }
    showStatus(tr("Liked: loading…"));
    QPointer<Player> guardedPlayer(corePlayer);
    const quint64 ticket = corePlayer->newSourceRequest();
    yandexLibrary->likedTracks([guardedPlayer, autoplay,
                                ticket](const QList<Track>& tracks, const QString& error) {
        if (!guardedPlayer || !guardedPlayer->isLatestSourceRequest(ticket)) {
            return;
        }
        if (!error.isEmpty()) {
            return ShowStatus(guardedPlayer, tr("Error: %1").arg(error));
        }
        if (!HasAvailable(tracks)) {  // SRC-07, SRC-08: the queue stays
            return ShowStatus(guardedPlayer, tr("Liked: empty"));
        }
        guardedPlayer->setQueue(tracks, tr("Liked"), autoplay);
        ShowStatus(
            guardedPlayer,
            tr("Liked: %n track(s)", nullptr, static_cast<int>(guardedPlayer->playlist().size()))
        );
    });
}

void Sources::playPlaylist(const Yandex::PlaylistReference& playlist) {
    if (refusedForJam()) {
        return;
    }
    yandexLibrary->playlistTracks(
        playlist, QueueLoader(corePlayer, playlist.title, corePlayer->newSourceRequest())
    );
}

void Sources::playRecommendations(const Yandex::PlaylistReference& playlist) {
    if (refusedForJam()) {
        return;
    }
    yandexLibrary->playlistRecommendations(
        playlist,
        QueueLoader(
            corePlayer, tr("%1: similar").arg(playlist.title), corePlayer->newSourceRequest()
        )
    );
}

void Sources::playArtist(const QString& artistId, const QString& name) {
    if (refusedForJam()) {
        return;
    }
    yandexLibrary->artistTopTracks(
        artistId, QueueLoader(corePlayer, name, corePlayer->newSourceRequest())
    );
}

void Sources::playAlbum(const QString& albumId, const QString& title) {
    if (refusedForJam()) {
        return;
    }
    yandexLibrary->albumTracks(
        albumId, QueueLoader(corePlayer, title, corePlayer->newSourceRequest())
    );
}

void Sources::playWave(const QStringList& seeds, const QString& title) {
    if (refusedForJam()) {
        return;
    }
    Yandex::Library* library = yandexLibrary;
    showStatus(tr("%1: loading…").arg(title));
    QPointer<Player> guardedPlayer(corePlayer);
    QPointer<Sources> self(this);
    const quint64 ticket = corePlayer->newSourceRequest();
    library->startWave(
        seeds,
        [self, guardedPlayer, library, title, ticket,
         seeds](const Yandex::WaveBatch& batch, const QString& error) {
            if (!guardedPlayer || !guardedPlayer->isLatestSourceRequest(ticket)) {
                return;
            }
            if (!error.isEmpty()) {
                return ShowStatus(guardedPlayer, tr("Vibe error: %1").arg(error));
            }
            if (!HasAvailable(batch.tracks)) {  // WAVE-03: no queue, no feedback
                return ShowStatus(guardedPlayer, tr("%1: empty").arg(title));
            }
            auto state = std::make_shared<WaveState>();
            state->session = batch.sessionId;
            state->station = seeds.value(0);
            for (const Track& track : batch.tracks) {
                state->batchOfTrack.insert(track.id, batch.batchId);
            }
            if (self) {
                self->waveSeeds = seeds;
            }
            library->waveFeedback(
                state->session, state->station, batch.batchId, Yandex::WaveEvent::RadioStarted
            );
            guardedPlayer->setQueue(
                batch.tracks, title, true, WaveMore(guardedPlayer, library, state),
                WaveEvents(library, state)
            );
        }
    );
}

void Sources::playMyWave() {
    playWave({kMyWaveSeed}, tr("My Vibe"));
}

void Sources::search(const QString& text) {
    if (refusedForJam()) {
        return;
    }
    Yandex::Library* library = yandexLibrary;
    const QString title = tr("Search: %1").arg(text);
    showStatus(title + QStringLiteral("…"));
    QPointer<Player> guardedPlayer(corePlayer);
    const quint64 ticket = corePlayer->newSourceRequest();
    library->search(
        text,
        [guardedPlayer, library, title,
         ticket](const Yandex::SearchResult& result, const QString& error) {
            if (!guardedPlayer || !guardedPlayer->isLatestSourceRequest(ticket)) {
                return;
            }
            if (!error.isEmpty()) {
                return ShowStatus(guardedPlayer, tr("Search error: %1").arg(error));
            }
            // The second request belongs to the same pick: a newer one drops it too (SRC-03).
            if (result.bestKind == Yandex::SearchResult::Kind::Artist && !result.bestId.isEmpty()) {
                return library->artistTopTracks(
                    result.bestId, QueueLoader(guardedPlayer, result.bestName, ticket)
                );
            }
            if (result.bestKind == Yandex::SearchResult::Kind::Album && !result.bestId.isEmpty()) {
                return library->albumTracks(
                    result.bestId, QueueLoader(guardedPlayer, result.bestName, ticket)
                );
            }
            if (!HasAvailable(result.tracks)) {  // SRC-12
                return ShowStatus(guardedPlayer, tr("Nothing found"));
            }
            guardedPlayer->setQueue(result.tracks, title, true);
        }
    );
}

void Sources::setLiked(const QString& trackId, bool liked) {
    QPointer<Player> guardedPlayer(corePlayer);
    yandexLibrary->setLiked(trackId, liked, [guardedPlayer, liked](bool, const QString& error) {
        if (!guardedPlayer) {
            return;
        }
        ShowStatus(
            guardedPlayer,
            !error.isEmpty() ? tr("Error: %1").arg(error)
                : liked      ? tr("Added to Liked")
                             : tr("Removed from Liked")
        );
    });
}

void Sources::dislikeAndSkip(const QString& trackId) {
    QPointer<Player> guardedPlayer(corePlayer);
    yandexLibrary->dislike(trackId, [guardedPlayer](bool, const QString& error) {
        if (!guardedPlayer) {
            return;
        }
        ShowStatus(guardedPlayer, error.isEmpty() ? tr("Disliked") : tr("Error: %1").arg(error));
    });
    if (const auto* current = corePlayer->currentTrack(); current && current->id == trackId) {
        corePlayer->next();
    }
}

}  // namespace Core
