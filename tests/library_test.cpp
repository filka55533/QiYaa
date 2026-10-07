#include "audio/audio_engine.h"
#include "core/player.h"
#include "support/mock_http_server.h"
#include "support/spec_fixtures.h"
#include "support/yandex_json.h"
#include "yandex/api_client.h"
#include "yandex/library.h"
#include "yandex/oauth.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QSet>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QUrl>
#include <QVariant>

#include <functional>

namespace {

// JSON with single quotes (moc can't parse raw string literals).
QByteArray J(const char* singleQuoted) {
    return QByteArray(singleQuoted).replace('\'', '"');
}

QByteArray TrackJson(int id, const char* title, int albumId = 0) {
    QByteArray json = "{\"id\":" + QByteArray::number(id) + ",\"title\":\"" + title
        + "\",\"artists\":[{\"name\":\"Artist\"}],\"durationMs\":180000";
    if (albumId) {
        json += ",\"albums\":[{\"id\":" + QByteArray::number(albumId) + "}]";
    }
    return json + "}";
}

// The ids an expected result lists under `key` ("trackIds", "albumIds").
QStringList ExpectedIds(const QString& name, const char* key) {
    QStringList ids;
    for (const QJsonValue& id : Tests::ExpectedObject(name).value(QLatin1String(key)).toArray()) {
        ids << id.toString();
    }
    return ids;
}

// The tracks of a wrapped /tracks/ reply in spec/fixtures/yandex.
QList<Yandex::Track> FixtureTracks(const QString& name) {
    QList<Yandex::Track> tracks;
    const QJsonObject reply = QJsonDocument::fromJson(Tests::Fixture(name)).object();
    for (const QJsonValue& value : reply.value(QStringLiteral("result")).toArray()) {
        tracks << Yandex::ApiClient::ParseTrack(value);
    }
    return tracks;
}

template <typename T>
struct Result {
    T value{};
    QString error;
    bool done = false;
    auto callback() {
        return [this](const T& receivedValue, const QString& receivedError) {
            value = receivedValue;
            error = receivedError;
            done = true;
        };
    }
    bool wait() {
        return QTest::qWaitFor([this] { return done; }, 5000);
    }
};

}  // namespace

class TestLibrary : public QObject {
    Q_OBJECT
private:
    Tests::MockHttpServer server;
    QNetworkAccessManager networkManager;
    Yandex::ApiClient api{&networkManager};
    Yandex::Library library{&api};

private Q_SLOTS:
    void initTestCase() {
        api.setBaseUrl(server.baseUrl());
        api.setToken(QStringLiteral("test-token"));
        server.fixture("GET", "/account/status", "account-status/ok");
    }

    void connectsAccountWithAuthHeader() {
        Result<Yandex::Account> account;
        library.connectAccount(account.callback());
        QVERIFY(account.wait());
        QVERIFY2(account.error.isEmpty(), qPrintable(account.error));
        QCOMPARE(Tests::Json(Tests::ToJson(account.value)), Tests::Expected("account-status/ok"));
        QVERIFY(library.isLoggedIn());
        QCOMPARE(
            server.last("/account/status")->headers.value("authorization"),
            QByteArray("OAuth test-token")
        );
    }

    void likedTracksFetchesMetadataAndRemembersLikes_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("string ids") << QStringLiteral("users-likes-tracks/string-ids");
        QTest::newRow("number ids") << QStringLiteral("users-likes-tracks/number-ids");
    }
    void likedTracksFetchesMetadataAndRemembersLikes() {
        QFETCH(QString, name);
        server.fixture("GET", "/users/42/likes/tracks", name);
        server.fixture("POST", "/tracks/", "tracks/two-tracks");
        Result<QList<Yandex::Track>> liked;
        library.likedTracks(liked.callback());
        QVERIFY(liked.wait());
        QVERIFY2(liked.error.isEmpty(), qPrintable(liked.error));
        const QStringList ids = ExpectedIds(name, "trackIds");
        QCOMPARE(server.last("/tracks/")->formValue("track-ids"), ids.join(u','));
        QCOMPARE(Tests::Json(Tests::TracksJson(liked.value)), Tests::Expected("tracks/two-tracks"));
        for (const QString& id : ids) {
            QVERIFY(library.isLiked(id));
        }
        QVERIFY(!library.isLiked("3"));
    }

    void userPlaylistsAndTheirEmbeddedTracksAreParsed() {
        server.fixture("GET", "/users/42/playlists/list", "users-playlists-list/ok");
        Result<QList<Yandex::PlaylistReference>> lists;
        library.userPlaylists(lists.callback());
        QVERIFY(lists.wait());
        QCOMPARE(
            Tests::Json(Tests::PlaylistsJson(lists.value)),
            Tests::Expected("users-playlists-list/ok")
        );

        // Embedded track objects are used directly.
        server.fixture("GET", "/users/42/playlists/1003", "users-playlists/embedded-tracks");
        const auto before = server.requests().size();
        Result<QList<Yandex::Track>> tracks;
        library.playlistTracks(lists.value[0], tracks.callback());
        QVERIFY(tracks.wait());
        QCOMPARE(
            Tests::Json(Tests::TracksJson(tracks.value)),
            Tests::Expected("users-playlists/embedded-tracks")
        );
        QCOMPARE(server.requests().size(), before + 1);  // no /tracks/ request
    }

    void playlistWithoutEmbeddedTracksFetchesByIds() {
        server.fixture("GET", "/users/7/playlists/3", "users-playlists/ids-only");
        server.fixture("POST", "/tracks/", "tracks/two-tracks");
        Result<QList<Yandex::Track>> tracks;
        library.playlistTracks(Yandex::PlaylistReference{"7", "3", "x", 2}, tracks.callback());
        QVERIFY(tracks.wait());
        QCOMPARE(
            server.last("/tracks/")->formValue("track-ids"),
            ExpectedIds("users-playlists/ids-only", "trackIds").join(u',')
        );
        QCOMPARE(
            Tests::Json(Tests::TracksJson(tracks.value)), Tests::Expected("tracks/two-tracks")
        );
    }

    void likedArtistsAndTheirTopTracksAreParsed() {
        server.fixture("GET", "/users/42/likes/artists", "users-likes-artists/ok");
        Result<QList<Yandex::NamedReference>> artists;
        library.likedArtists(artists.callback());
        QVERIFY(artists.wait());
        QCOMPARE(
            Tests::Json(Tests::NamedJson("artists", artists.value)),
            Tests::Expected("users-likes-artists/ok")
        );

        server.fixture("POST", "/tracks/", "tracks/two-tracks");
        for (const char* name :
             {"artists-track-ids-by-rating/ok", "artists-track-ids-by-rating/more-than-100"}) {
            server.fixture("GET", "/artists/9/track-ids-by-rating", name);
            Result<QList<Yandex::Track>> top;
            library.artistTopTracks("9", top.callback());
            QVERIFY(top.wait());
            QVERIFY2(top.error.isEmpty(), qPrintable(top.error));
            QCOMPARE(
                server.last("/tracks/")->formValue("track-ids"),
                ExpectedIds(name, "trackIds").join(u',')
            );
        }
    }

    void albumsSkipPodcasts() {
        server.fixture("GET", "/users/42/likes/albums", "users-likes-albums/ok");
        server.fixture("POST", "/albums", "albums/with-podcast");
        Result<QList<Yandex::NamedReference>> albums;
        library.likedAlbums(albums.callback());
        QVERIFY(albums.wait());
        QCOMPARE(
            server.last("/albums")->formValue("album-ids"),
            ExpectedIds("users-likes-albums/ok", "albumIds").join(u',')
        );
        QCOMPARE(
            Tests::Json(Tests::NamedJson("albums", albums.value)),
            Tests::Expected("albums/with-podcast")
        );

        server.fixture("GET", "/albums/4053/with-tracks", "albums-with-tracks/two-volumes");
        Result<QList<Yandex::Track>> tracks;
        library.albumTracks("4053", tracks.callback());
        QVERIFY(tracks.wait());
        QCOMPARE(
            Tests::Json(Tests::TracksJson(tracks.value)),
            Tests::Expected("albums-with-tracks/two-volumes")
        );
    }

    void stationsAreListedWithTypeTagIds() {
        server.fixture("GET", "/rotor/stations/list", "rotor-stations-list/ok");
        Result<QList<Yandex::Station>> stationList;
        library.stations(stationList.callback());
        QVERIFY(stationList.wait());
        QCOMPARE(
            Tests::Json(Tests::StationsJson(stationList.value)),
            Tests::Expected("rotor-stations-list/ok")
        );
        QCOMPARE(
            server.last("/rotor/stations/list")->query.queryItemValue("language"),
            QStringLiteral("ru")
        );
    }

    void waveSessionStartsFromSeedsAndContinuesWithTheQueue() {
        server.fixture("POST", "/rotor/session/new", "rotor-session-new/ok");
        Result<Yandex::WaveBatch> first;
        library.startWave({"user:onyourwave"}, first.callback());
        QVERIFY(first.wait());
        QCOMPARE(Tests::Json(Tests::ToJson(first.value)), Tests::Expected("rotor-session-new/ok"));
        const QJsonObject body =
            QJsonDocument::fromJson(server.last("/rotor/session/new")->body).object();
        QCOMPARE(
            body.value("seeds").toArray().first().toString(), QStringLiteral("user:onyourwave")
        );
        QVERIFY(body.value("includeTracksInResponse").toBool());

        const QString sessionId = first.value.sessionId;
        server.fixture(
            "POST", QStringLiteral("/rotor/session/%1/tracks").arg(sessionId),
            "rotor-session-tracks/ok"
        );
        Result<Yandex::WaveBatch> more;
        library.moreWave(sessionId, {"38634572"}, more.callback());
        QVERIFY(more.wait());
        QCOMPARE(
            Tests::Json(Tests::ToJson(more.value)), Tests::Expected("rotor-session-tracks/ok")
        );
        const QJsonObject moreBody =
            QJsonDocument::fromJson(
                server.last(QStringLiteral("/rotor/session/%1/tracks").arg(sessionId))->body
            )
                .object();
        QCOMPARE(moreBody.value("queue").toArray().first().toString(), QStringLiteral("38634572"));
    }

    void searchReportsTheBestResult_data() {
        QTest::addColumn<QString>("name");
        for (const char* name :
             {"best-artist", "best-album", "best-track", "best-playlist", "best-podcast", "no-best"
             }) {
            QTest::newRow(name) << QStringLiteral("search/%1").arg(name);
        }
    }
    void searchReportsTheBestResult() {
        QFETCH(QString, name);
        server.fixture("GET", "/search", name);
        Result<Yandex::SearchResult> found;
        library.search("кино", found.callback());
        QVERIFY(found.wait());
        QCOMPARE(Tests::Json(Tests::ToJson(found.value)), Tests::Expected(name));
        QCOMPARE(server.last("/search")->query.queryItemValue("text"), QStringLiteral("кино"));
    }

    void searchTracksAsksForTracksOnlyAndTellsARejectedToken() {  // spec HOST-28, HOST-29
        server.fixture("GET", "/search", "search/best-track");
        Result<Yandex::SearchResult> all;
        library.search("кино", all.callback());
        QVERIFY(all.wait());
        QList<Yandex::Track> tracks;
        Yandex::RequestError failure;
        bool done = false;
        library.searchTracks(
            "кино",
            [&](const QList<Yandex::Track>& found, const Yandex::RequestError& error) {
                tracks = found;
                failure = error;
                done = true;
            }
        );
        QVERIFY(QTest::qWaitFor([&] { return done; }, 5000));
        QVERIFY(!failure.isError());
        QCOMPARE(server.last("/search")->query.queryItemValue("type"), QStringLiteral("track"));
        QCOMPARE(tracks.size(), all.value.tracks.size());
        QCOMPARE(tracks.first().id, all.value.tracks.first().id);

        server.fixture("GET", "/search", "users-likes-artists/401-session-expired");
        done = false;
        library.searchTracks(
            "кино",
            [&](const QList<Yandex::Track>&, const Yandex::RequestError& error) {
                failure = error;
                done = true;
            }
        );
        QVERIFY(QTest::qWaitFor([&] { return done; }, 5000));
        QCOMPARE(failure.httpStatus, 401);
    }

    void likeUnlikeAndDislikeUpdateTheLikes() {
        server.fixture(
            "POST", "/users/42/likes/tracks/add-multiple", "users-likes-tracks-add-multiple/ok"
        );
        server.fixture("POST", "/users/42/likes/tracks/remove", "users-likes-tracks-remove/ok");
        server.fixture(
            "POST", "/users/42/dislikes/tracks/add-multiple",
            "users-dislikes-tracks-add-multiple/ok"
        );
        Result<bool> like;
        library.setLiked("77", true, like.callback());
        QVERIFY(like.wait());
        QVERIFY(library.isLiked("77"));
        QCOMPARE(
            server.last("/users/42/likes/tracks/add-multiple")->formValue("track-ids"),
            QStringLiteral("77")
        );
        Result<bool> unlike;
        library.setLiked("77", false, unlike.callback());
        QVERIFY(unlike.wait());
        QVERIFY(!library.isLiked("77"));
        Result<bool> dislike;
        library.dislike("33311009", dislike.callback());
        QVERIFY(dislike.wait());
        QVERIFY(dislike.value);
        QVERIFY(!library.isLiked("33311009"));
    }

    void errorsAreReported_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("401 error object")
            << QStringLiteral("users-likes-artists/401-session-expired");
        QTest::newRow("404 error object") << QStringLiteral("rotor-session-feedback/404-not-found");
        QTest::newRow("503 error string")
            << QStringLiteral("rotor-session-feedback/503-string-error");
        QTest::newRow("500 empty body") << QStringLiteral("account-status/500-empty");
    }
    void errorsAreReported() {
        QFETCH(QString, name);
        server.fixture("GET", "/users/42/likes/artists", name);
        Result<QList<Yandex::NamedReference>> artists;
        library.likedArtists(artists.callback());
        QVERIFY(artists.wait());
        const QString problem = Tests::CheckError(artists.error, name);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
    }

    void deviceLoginPollsUntilToken() {
        server.fixture("POST", "/device/code", "oauth-device-code/ok");
        int polls = 0;
        server.on("POST", "/token", [&polls](const Tests::MockRequest& request) {
            auto answer = [](const QString& name) {
                return Tests::MockResponse{Tests::FixtureStatus(name), Tests::Fixture(name)};
            };
            if (request.formValue("code") != "DEV") {
                return answer("oauth-token/400-bad-verification-code");
            }
            if (++polls < 2) {
                return answer("oauth-token/400-authorization-pending");
            }
            return answer("oauth-token/ok");
        });
        const QJsonObject code = Tests::ExpectedObject("oauth-device-code/ok");
        Yandex::DeviceLogin login(&networkManager);
        login.setBaseUrl(server.baseUrl());
        QSignalSpy codeReady(&login, &Yandex::DeviceLogin::codeReady);
        QSignalSpy succeeded(&login, &Yandex::DeviceLogin::succeeded);
        login.start();
        QVERIFY(codeReady.wait(3000));
        QCOMPARE(codeReady.first().at(0).toString(), code.value("userCode").toString());
        QCOMPARE(codeReady.first().at(1).toUrl(), QUrl(code.value("verificationUrl").toString()));
        QVERIFY(succeeded.wait(5000));
        QCOMPARE(
            succeeded.first().at(0).toString(),
            Tests::ExpectedObject("oauth-token/ok").value("accessToken").toString()
        );
        QCOMPARE(polls, 2);
        QCOMPARE(server.last("/token")->formValue("grant_type"), QStringLiteral("device_code"));
    }

    void deviceLoginFailureIsReported() {
        const QString name = QStringLiteral("oauth-device-code/400-invalid-client");
        server.fixture("POST", "/device/code", name);
        Yandex::DeviceLogin login(&networkManager);
        login.setBaseUrl(server.baseUrl());
        QSignalSpy failed(&login, &Yandex::DeviceLogin::failed);
        login.start();
        QVERIFY(failed.wait(3000));
        QCOMPARE(
            failed.first().at(0).toString(),
            Tests::ExpectedObject(name).value("error").toObject().value("message").toString()
        );
    }

    void personalPlaylistsAndTheirRecommendationsAreParsed() {
        server.fixture("GET", "/landing3", "landing3/personal-playlists");
        Result<QList<Yandex::PlaylistReference>> playlists;
        library.personalPlaylists(playlists.callback());
        QVERIFY(playlists.wait());
        QVERIFY2(playlists.error.isEmpty(), qPrintable(playlists.error));
        QCOMPARE(
            Tests::Json(Tests::PlaylistsJson(playlists.value)),
            Tests::Expected("landing3/personal-playlists")
        );
        QCOMPARE(
            server.last("/landing3")->query.queryItemValue("blocks"),
            QStringLiteral("personalplaylists")
        );

        server.fixture(
            "GET", "/users/503646255/playlists/123/recommendations",
            "users-playlists-recommendations/ok"
        );
        Result<QList<Yandex::Track>> recommendations;
        library.playlistRecommendations(playlists.value[0], recommendations.callback());
        QVERIFY(recommendations.wait());
        QCOMPARE(
            Tests::Json(Tests::TracksJson(recommendations.value)),
            Tests::Expected("users-playlists-recommendations/ok")
        );
    }

    void wheelOfWavesReadsAnUnwrappedBodyAndSkipsOtherItems() {
        server.fixture("POST", "/wheel/new", "wheel-new/ok");
        Result<QList<Yandex::Wave>> waves;
        library.wheelWaves({"user:onyourwave"}, waves.callback());
        QVERIFY(waves.wait());
        QVERIFY2(waves.error.isEmpty(), qPrintable(waves.error));
        QCOMPARE(Tests::Json(Tests::WavesJson(waves.value)), Tests::Expected("wheel-new/ok"));
        const QJsonObject body = QJsonDocument::fromJson(server.last("/wheel/new")->body).object();
        QCOMPARE(body.value("context").toObject().value("type").toString(), QStringLiteral("WAVE"));
    }

    void waveFeedbackFallsBackToStationEndpoint() {
        const Yandex::Track track = FixtureTracks("tracks/two-tracks").first();
        // Session endpoint works: only it is used.
        server.fixture("POST", "/rotor/session/OK1/feedback", "rotor-session-feedback/ok");
        library.waveFeedback(
            "OK1", "user:onyourwave", "B1", Yandex::WaveEvent::TrackStarted, &track
        );
        QVERIFY(QTest::qWaitFor(
            [&] { return server.last("/rotor/session/OK1/feedback") != nullptr; }, 3000
        ));
        const QJsonObject sessionBody =
            QJsonDocument::fromJson(server.last("/rotor/session/OK1/feedback")->body).object();
        QCOMPARE(sessionBody.value("batchId").toString(), QStringLiteral("B1"));
        QCOMPARE(
            sessionBody.value("event").toObject().value("type").toString(),
            QStringLiteral("trackStarted")
        );
        QCOMPARE(
            sessionBody.value("event").toObject().value("trackId").toString(),
            track.id + u':' + track.albumId
        );

        // Session endpoint rejected: falls back to the station endpoint, and stays there.
        server.fixture(
            "POST", "/rotor/session/BAD/feedback", "rotor-session-feedback/404-not-found"
        );
        server.fixture(
            "POST", "/rotor/station/user:onyourwave/feedback", "rotor-station-feedback/ok"
        );
        const auto before = server.requests().size();
        auto stationCalls = [&] {
            int calls = 0;
            for (qsizetype i = before; i < server.requests().size(); ++i) {
                calls += server.requests()[i].path == "/rotor/station/user:onyourwave/feedback";
            }
            return calls;
        };
        library.waveFeedback(
            "BAD", "user:onyourwave", "B2", Yandex::WaveEvent::Skip, &track, 12.34
        );
        QVERIFY(QTest::qWaitFor([&] { return stationCalls() == 1; }, 3000));
        const Tests::MockRequest* stationRequest =
            server.last("/rotor/station/user:onyourwave/feedback");
        QCOMPARE(stationRequest->query.queryItemValue("batch-id"), QStringLiteral("B2"));
        const QJsonObject stationBody = QJsonDocument::fromJson(stationRequest->body).object();
        QCOMPARE(stationBody.value("type").toString(), QStringLiteral("skip"));
        QCOMPARE(stationBody.value("totalPlayedSeconds").toDouble(), 12.3);
        library.waveFeedback(
            "BAD", "user:onyourwave", "B2", Yandex::WaveEvent::TrackStarted, &track
        );
        QVERIFY(QTest::qWaitFor([&] { return stationCalls() == 2; }, 3000));
        int sessionCalls = 0;
        for (qsizetype i = before; i < server.requests().size(); ++i) {
            sessionCalls += server.requests()[i].path == "/rotor/session/BAD/feedback";
        }
        QCOMPARE(sessionCalls, 1);
    }

    void waveFeedbackDoesNotResendAfterServerError() {
        const Yandex::Track track = FixtureTracks("tracks/two-tracks").first();
        server.fixture(
            "POST", "/rotor/session/S500/feedback", "rotor-session-feedback/503-string-error"
        );
        const auto before = server.requests().size();
        bool settled = false;
        const int pending = api.pendingPosts();
        auto connection =
            connect(&api, &Yandex::ApiClient::postsSettled, this, [&] { settled = true; });
        library.waveFeedback("S500", "user:onyourwave", "B1", Yandex::WaveEvent::Skip, &track, 3);
        QCOMPARE(api.pendingPosts(), pending + 1);
        QVERIFY(QTest::qWaitFor([&] { return settled; }, 3000));
        disconnect(connection);
        for (qsizetype i = before; i < server.requests().size(); ++i) {
            QVERIFY(!server.requests()[i].path.startsWith("/rotor/station/"));
        }
    }

    void postsSettleOnlyAfterTheFallback() {
        const Yandex::Track track = FixtureTracks("tracks/two-tracks").first();
        server.fixture(
            "POST", "/rotor/session/S404/feedback", "rotor-session-feedback/404-not-found"
        );
        server.fixture("POST", "/rotor/station/user:x/feedback", "rotor-station-feedback/ok");
        const auto before = server.requests().size();
        int stationCallsAtSettle = -1;
        auto stationCalls = [&] {
            int calls = 0;
            for (qsizetype i = before; i < server.requests().size(); ++i) {
                calls += server.requests()[i].path == "/rotor/station/user:x/feedback";
            }
            return calls;
        };
        auto connection = connect(&api, &Yandex::ApiClient::postsSettled, this, [&] {
            stationCallsAtSettle = stationCalls();
        });
        library.waveFeedback("S404", "user:x", "B1", Yandex::WaveEvent::Skip, &track, 3);
        QVERIFY(QTest::qWaitFor([&] { return stationCallsAtSettle >= 0; }, 3000));
        QCOMPARE(stationCallsAtSettle, 1);
        QCOMPARE(api.pendingPosts(), 0);
        disconnect(connection);
    }

    void playerReportsTrackEvents() {
        Audio::AudioEngine engine;
        Core::Player player(&library, &engine);
        server.result(
            "GET", "/tracks/1/download-info",
            "[{\"codec\":\"mp3\",\"bitrateInKbps\":320,\"downloadInfoUrl\":\""
                + server.baseUrl().toUtf8() + "/dl?x=1\"}]"
        );
        server.result(
            "GET", "/tracks/2/download-info",
            "[{\"codec\":\"mp3\",\"bitrateInKbps\":320,\"downloadInfoUrl\":\""
                + server.baseUrl().toUtf8() + "/dl?x=2\"}]"
        );
        server.json("GET", "/dl", J("{'host':'127.0.0.1:1','path':'/p','ts':'1','s':'s'}"));
        server.fixture("POST", "/play-audio", "play-audio/ok");
        QList<Yandex::Track> tracks;
        for (int i = 1; i <= 2; ++i) {
            tracks << Yandex::ApiClient::ParseTrack(
                QJsonDocument::fromJson(TrackJson(i, "t")).object()
            );
        }
        QStringList log;
        player.setQueue(
            tracks, "W", false, {},
            [&](Core::Player::TrackEvent event, const Yandex::Track& track, double) {
                log << QStringLiteral("%1:%2").arg(static_cast<int>(event)).arg(track.id);
            }
        );
        player.playIndex(0);
        QVERIFY(QTest::qWaitFor([&] { return log.contains("0:1"); }, 3000));
        player.playIndex(1);
        QVERIFY(QTest::qWaitFor([&] { return log.contains("0:2"); }, 3000));
        QCOMPARE(log, (QStringList{"0:1", "2:1", "0:2"}));
        player.setQueue({}, "", false);
        QCOMPARE(log.last(), QStringLiteral("2:2"));
    }

private:
    struct PlaybackStack {
        Tests::LocalNetworkAccessManager networkManager;
        Yandex::ApiClient api{&networkManager};
        Yandex::Library library{&api};
        Audio::AudioEngine engine;
        Core::Player player{&library, &engine};
    };
    bool setUpAudio(PlaybackStack& playback, const QList<int>& ids) {
        QFile file(QStringLiteral(QIYAA_TEST_DATA "/sine440_3s.mp3"));
        if (!file.open(QIODevice::ReadOnly) || !playback.engine.init().ok) {
            return false;
        }
        playback.engine.setVolume(0);
        playback.api.setBaseUrl(server.baseUrl());
        QStringList trackIds;
        for (int id : ids) {
            trackIds << QString::number(id);
        }
        server.audioTracks(trackIds, file.readAll());
        return true;
    }
    int requestsTo(const QString& path) const {
        int count = 0;
        for (const Tests::MockRequest& request : server.requests()) {
            count += request.path == path;
        }
        return count;
    }
    static QList<Yandex::Track> NumberedTracks(const QList<int>& ids) {
        QList<Yandex::Track> tracks;
        for (int id : ids) {
            tracks << Yandex::ApiClient::ParseTrack(
                QJsonDocument::fromJson(TrackJson(id, "t")).object()
            );
        }
        return tracks;
    }

private Q_SLOTS:
    void playerPreloadsAndAdvancesSeamlessly() {
        PlaybackStack playback;
        if (!setUpAudio(playback, {11, 12, 13})) {
            QSKIP("no audio output");
        }
        QStringList log;
        playback.player.setQueue(
            NumberedTracks({11, 12, 13}), "A", false, {},
            [&](Core::Player::TrackEvent event, const Yandex::Track& track, double) {
                log << QStringLiteral("%1:%2").arg(static_cast<int>(event)).arg(track.id);
            }
        );
        QSignalSpy advanced(&playback.engine, &Audio::AudioEngine::trackAdvanced);
        QSignalSpy finished(&playback.engine, &Audio::AudioEngine::trackFinished);
        playback.player.playIndex(0);
        QVERIFY(QTest::qWaitFor([&] { return playback.player.preloadedIndex() == 1; }, 5000));
        QVERIFY(QTest::qWaitFor(
            [&] { return playback.engine.state() == Audio::AudioEngine::State::Playing; }, 3000
        ));
        QVERIFY(playback.player.currentLink().path().endsWith(QStringLiteral("/t11")));
        QVERIFY(playback.player.nextLink().path().endsWith(QStringLiteral("/t12")));  // LISTEN-03
        QTest::qWait(300);  // let the preload download complete
        QVERIFY(playback.player.seekTo(2.4));
        QVERIFY(advanced.wait(4000));
        QCOMPARE(finished.count(), 0);
        QCOMPARE(playback.player.currentIndex(), 1);
        QCOMPARE(log, (QStringList{"0:11", "1:11", "0:12"}));
        QCOMPARE(requestsTo("/tracks/12/download-info"), 1);
        QVERIFY(playback.engine.positionSeconds() < 0.5);
        // The link of the file that plays follows the gapless advance (listening along).
        QVERIFY(playback.player.currentLink().path().endsWith(QStringLiteral("/t12")));

        // "Next" takes the preloaded track too.
        QVERIFY(QTest::qWaitFor([&] { return playback.player.preloadedIndex() == 2; }, 5000));
        playback.player.next();
        QCOMPARE(playback.player.currentIndex(), 2);
        QCOMPARE(log.mid(3), (QStringList{"2:12", "0:13"}));
        QVERIFY(playback.player.currentLink().path().endsWith(QStringLiteral("/t13")));
        QCOMPARE(requestsTo("/tracks/13/download-info"), 1);
        playback.player.stop();
    }

    void playerRepreloadsWhenTheQueueChanges() {
        PlaybackStack playback;
        if (!setUpAudio(playback, {21, 22, 23})) {
            QSKIP("no audio output");
        }
        playback.player.setQueue(NumberedTracks({21, 22, 23}), "A", false);
        playback.player.playIndex(0);
        QVERIFY(QTest::qWaitFor([&] { return playback.player.preloadedIndex() == 1; }, 5000));
        playback.player.removeTracks({1});  // the preloaded track is gone: 23 follows now
        QVERIFY(QTest::qWaitFor(
            [&] {
                return playback.player.preloadedIndex() == 1
                    && requestsTo("/tracks/23/download-info") == 1;
            },
            5000
        ));
        QTest::qWait(300);
        QSignalSpy advanced(&playback.engine, &Audio::AudioEngine::trackAdvanced);
        QVERIFY(playback.player.seekTo(2.4));
        QVERIFY(advanced.wait(4000));
        QCOMPARE(playback.player.currentTrack()->id, QStringLiteral("23"));
        playback.player.stop();
    }

    void playerAsksEndlessSourceForMore() {
        Audio::AudioEngine engine;  // not initialised: nothing actually plays
        Core::Player player(&library, &engine);
        QList<Yandex::Track> batch;
        for (int i = 0; i < 3; ++i) {
            batch << Yandex::ApiClient::ParseTrack(
                QJsonDocument::fromJson(TrackJson(i + 1, "t")).object()
            );
        }
        int asked = 0;
        player.setQueue(
            batch, "Wave", false,
            [&](std::function<void(const QList<Yandex::Track>&)> done) {
                ++asked;
                done({Yandex::ApiClient::ParseTrack(
                    QJsonDocument::fromJson(TrackJson(99, "more")).object()
                )});
            }
        );
        QCOMPARE(player.playlist().size(), 3);
        player.playIndex(1);  // 2 tracks left -> ask for more
        QCOMPARE(asked, 1);
        QCOMPARE(player.playlist().size(), 4);
        QCOMPARE(player.playlist().last().title, QStringLiteral("more"));
    }

    void previousFollowsTheThreeSecondRule_data() {  // spec TR-01, TR-02
        QTest::addColumn<double>("position");
        QTest::addColumn<int>("cursor");
        QTest::addColumn<bool>("repeat");
        QTest::addColumn<int>("target");
        QTest::newRow("early: previous track") << 1.0 << 2 << false << 1;
        QTest::newRow("at 3 s: still previous") << 3.0 << 2 << false << 1;
        QTest::newRow("late: restart") << 3.5 << 2 << false << -1;
        QTest::newRow("late on the first track: restart") << 10.0 << 0 << true << -1;
        QTest::newRow("first track, repeat: last") << 0.5 << 0 << true << 4;
        QTest::newRow("first track, no repeat: first") << 0.5 << 0 << false << 0;
    }
    void previousFollowsTheThreeSecondRule() {
        QFETCH(double, position);
        QFETCH(int, cursor);
        QFETCH(bool, repeat);
        QFETCH(int, target);
        QCOMPARE(Core::PreviousTarget(position, cursor, 5, repeat), target);
    }

    void repeatDoesNotReplayTheOnlyTrackOfAWave() {  // spec WAVE-12
        PlaybackStack playback;
        if (!setUpAudio(playback, {21})) {
            QSKIP("no audio output");
        }
        playback.player.setRepeat(true);
        playback.player.setQueue(
            NumberedTracks({21}), "Wave", false,
            [](std::function<void(const QList<Yandex::Track>&)>) {}
        );
        QSignalSpy finished(&playback.engine, &Audio::AudioEngine::trackFinished);
        playback.player.playIndex(0);
        QVERIFY(QTest::qWaitFor(
            [&] { return playback.engine.state() == Audio::AudioEngine::State::Playing; }, 8000
        ));
        QTest::qWait(300);  // the whole file has arrived
        QVERIFY(playback.player.seekTo(2.5));
        QVERIFY(finished.wait(4000));
        QVERIFY(QTest::qWaitFor([&] { return playback.player.isWaitingForMore(); }, 2000));
        QTest::qWait(300);
        QCOMPARE(playback.engine.state(), Audio::AudioEngine::State::Stopped);
        QVERIFY(!playback.player.trackInProgress());
    }

    void shuffleVisitsEachTrackOnceAndStops() {  // proposed spec SHUF-01 (docs/shuffle-spec.patch)
        Audio::AudioEngine engine;
        Core::Player player(&library, &engine);
        player.setShuffle(true);
        player.setQueue(NumberedTracks({1, 2, 3, 4, 5, 6}), "Plain", false);
        QSet<int> visited;
        for (int count = 0; count < 6; ++count) {
            QVERIFY(!visited.contains(player.currentIndex()));
            visited.insert(player.currentIndex());
            if (count < 5) {
                player.next();
            }
        }
        QCOMPARE(visited.size(), 6);
        const int last = player.currentIndex();
        QSignalSpy changed(&player, &Core::Player::currentTrackChanged);
        player.next();
        QCOMPARE(player.currentIndex(), last);
        QCOMPARE(changed.count(), 0);
        QVERIFY(!player.trackInProgress());
        for (int index = 0; index < 6; ++index) {
            QCOMPARE(player.playlist()[index].id, QString::number(index + 1));
        }
    }

    void shufflePreviousAndRepeatFollowTheStoredOrder(
    ) {  // proposed spec SHUF-01, SHUF-02 (docs/shuffle-spec.patch)
        Audio::AudioEngine engine;
        Core::Player player(&library, &engine);
        player.setQueue(NumberedTracks({1, 2, 3, 4, 5}), "Plain", false);
        player.setShuffle(true);
        player.setRepeat(true);
        QList<int> order{player.currentIndex()};
        for (int count = 1; count < 5; ++count) {
            player.next();
            order << player.currentIndex();
        }
        QCOMPARE(QSet<int>(order.begin(), order.end()).size(), 5);
        player.next();
        QCOMPARE(player.currentIndex(), order[0]);
        player.previous();
        QCOMPARE(player.currentIndex(), order[4]);
        for (int position = 3; position >= 0; --position) {
            player.previous();
            QCOMPARE(player.currentIndex(), order[position]);
        }
        player.playIndex(order[2]);
        player.next();
        QCOMPARE(player.currentIndex(), order[3]);
        player.setRepeat(false);
        player.next();
        QCOMPARE(player.currentIndex(), order[4]);
        player.next();
        QCOMPARE(player.currentIndex(), order[4]);
        QVERIFY(!player.trackInProgress());
    }

    void shuffleRebuildsAfterQueueEdits_data() {
        QTest::addColumn<QString>("operation");
        for (const char* operation : {"append", "insert", "remove", "remove current"}) {
            QTest::newRow(operation) << QString::fromLatin1(operation);
        }
    }

    void shuffleRebuildsAfterQueueEdits() {  // proposed spec SHUF-04 (docs/shuffle-spec.patch)
        QFETCH(QString, operation);
        Audio::AudioEngine engine;
        Core::Player player(&library, &engine);
        player.setQueue(NumberedTracks({1, 2, 3, 4}), "Plain", false);
        player.playIndex(2);
        player.setShuffle(true);
        if (operation == "append") {
            player.appendTracks(NumberedTracks({5}));
        } else if (operation == "insert") {
            player.insertTracks(0, NumberedTracks({5}));
        } else if (operation == "remove") {
            player.removeTracks({0});
        } else {
            player.removeTracks({2});
        }
        if (operation != "remove current") {
            QCOMPARE(player.currentTrack()->id, QStringLiteral("3"));
        }
        QSet<QString> visited;
        const int size = static_cast<int>(player.playlist().size());
        for (int count = 0; count < size; ++count) {
            const QString id = player.currentTrack()->id;
            QVERIFY(!visited.contains(id));
            visited.insert(id);
            if (count + 1 < size) {
                player.next();
            }
        }
        QCOMPARE(visited.size(), size);
        player.next();
        QVERIFY(!player.trackInProgress());
        player.removeTracks({0, 1, 2, 3, 4});
        player.next();
        player.previous();
        QCOMPARE(player.currentIndex(), -1);
    }

    void shuffleToggleKeepsPlaybackAndGaplessAdvanceFollowsTheOrder_data() {
        QTest::addColumn<int>("algorithm");
        QTest::newRow("without repeats")
            << static_cast<int>(Core::ShuffleAlgorithm::WithoutRepeats);
        QTest::newRow("random") << static_cast<int>(Core::ShuffleAlgorithm::Random);
    }

    void shuffleToggleKeepsPlaybackAndGaplessAdvanceFollowsTheOrder(
    ) {  // proposed spec SHUF-03, SHUF-05 (docs/shuffle-spec.patch)
        QFETCH(int, algorithm);
        PlaybackStack playback;
        if (!setUpAudio(playback, {31, 32, 33})) {
            QSKIP("no audio output");
        }
        auto& player = playback.player;
        player.setShuffleAlgorithm(static_cast<Core::ShuffleAlgorithm>(algorithm));
        player.setQueue(NumberedTracks({31, 32, 33}), "Plain", true);
        QVERIFY(QTest::qWaitFor([&] { return player.preloadedIndex() == 1; }, 5000));
        QVERIFY(player.seekTo(0.5));
        player.pause();
        const double position = playback.engine.positionSeconds();
        QSignalSpy changed(&player, &Core::Player::currentTrackChanged);
        player.setShuffle(true);
        QCOMPARE(player.currentIndex(), 0);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(playback.engine.state(), Audio::AudioEngine::State::Paused);
        QCOMPARE(playback.engine.positionSeconds(), position);
        QVERIFY(QTest::qWaitFor([&] { return player.preloadedIndex() >= 0; }, 5000));
        const int next = player.preloadedIndex();
        QVERIFY(next != 0);
        QTest::qWait(300);
        player.play();
        QSignalSpy advanced(&playback.engine, &Audio::AudioEngine::trackAdvanced);
        QVERIFY(player.seekTo(2.4));
        QVERIFY(advanced.wait(4000));
        QCOMPARE(player.currentIndex(), next);
        QVERIFY(QTest::qWaitFor([&] { return player.preloadedIndex() >= 0; }, 5000));
        const int last = player.preloadedIndex();
        QVERIFY(last != next);
        if (player.shuffleAlgorithm() == Core::ShuffleAlgorithm::WithoutRepeats) {
            QVERIFY(last != 0);
        }
        player.next();
        QCOMPARE(player.currentIndex(), last);
        if (player.shuffleAlgorithm() == Core::ShuffleAlgorithm::WithoutRepeats) {
            QCOMPARE(player.preloadedIndex(), -1);
        }
        player.pause();
        const double lastPosition = playback.engine.positionSeconds();
        changed.clear();
        player.setShuffleAlgorithm(
            player.shuffleAlgorithm() == Core::ShuffleAlgorithm::Random
                ? Core::ShuffleAlgorithm::WithoutRepeats
                : Core::ShuffleAlgorithm::Random
        );
        QCOMPARE(player.currentIndex(), last);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(playback.engine.state(), Audio::AudioEngine::State::Paused);
        QCOMPARE(playback.engine.positionSeconds(), lastPosition);
        player.setShuffle(false);
        QCOMPARE(player.currentIndex(), last);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(playback.engine.state(), Audio::AudioEngine::State::Paused);
        QCOMPARE(playback.engine.positionSeconds(), lastPosition);
        player.stop();
    }

    void shuffleHandlesEmptyAndSingleTrackQueues(
    ) {  // proposed spec SHUF-01, SHUF-04 (docs/shuffle-spec.patch)
        Audio::AudioEngine engine;
        Core::Player player(&library, &engine);
        player.setShuffle(true);
        player.setQueue({}, "Empty", false);
        player.next();
        player.previous();
        QCOMPARE(player.currentIndex(), -1);
        player.setQueue(NumberedTracks({1}), "Single", false);
        player.playIndex(0);
        player.next();
        QVERIFY(!player.trackInProgress());
        player.setRepeat(true);
        player.next();
        QCOMPARE(player.currentIndex(), 0);
        QVERIFY(player.trackInProgress());
    }

    void shuffleDoesNotApplyInAWave() {  // spec WAVE-10, WAVE-11
        Audio::AudioEngine engine;  // not initialised: nothing actually plays
        Core::Player player(&library, &engine);
        QSignalSpy status(&player, &Core::Player::statusMessage);
        auto shuffleMessages = [&] {
            int count = 0;
            for (const QList<QVariant>& arguments : status) {
                count += arguments.at(0).toString().startsWith(QStringLiteral("Shuffle"));
            }
            return count;
        };
        player.setShuffle(true);
        QList<int> ids;
        for (int i = 1; i <= 12; ++i) {
            ids << i;
        }
        int asked = 0;
        player.setQueue(
            NumberedTracks(ids), "Wave", false,
            [&](std::function<void(const QList<Yandex::Track>&)>) { ++asked; }
        );
        QCOMPARE(shuffleMessages(), 1);
        QVERIFY(player.shuffle());
        QVERIFY(!player.shuffleActive());
        player.playIndex(0);
        for (int expected = 1; expected <= 10; ++expected) {
            player.next();
            QCOMPARE(player.currentIndex(), expected);  // in order, not a random pick
        }
        QCOMPARE(asked, 1);  // 2 tracks left at index 10

        player.setShuffle(false);
        player.setShuffle(true);
        QCOMPARE(shuffleMessages(), 2);

        player.setQueue(NumberedTracks(ids), "Plain", false);
        QVERIFY(player.shuffle());
        QVERIFY(player.shuffleActive());
        QCOMPARE(shuffleMessages(), 2);
        player.stop();
    }
};

QTEST_GUILESS_MAIN(TestLibrary)
#include "library_test.moc"
