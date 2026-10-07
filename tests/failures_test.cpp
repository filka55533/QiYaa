#include "audio/audio_engine.h"
#include "core/failure_policy.h"
#include "core/player.h"
#include "support/mock_http_server.h"
#include "yandex/api_client.h"
#include "yandex/library.h"

#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QUrl>
#include <QVariant>

#include <functional>
#include <memory>

// What the Player does when a track cannot play: spec/player/errors.md, scenario by scenario.
// The policy is checked as a table; the scenarios play on miniaudio's Null output.

using Core::FailureAction;
using Core::FailureKind;

class TestFailures : public QObject {
    Q_OBJECT
private:
    Tests::MockHttpServer server;
    QByteArray mp3;
    // Per track id, what its /get-mp3/ link answers instead of the whole mp3.
    QHash<QString, Tests::MockResponse> streamOverride;

    struct Stack {
        Tests::LocalNetworkAccessManager networkManager;
        Yandex::ApiClient api{&networkManager};
        Yandex::Library library{&api};
        Audio::AudioEngine engine;
        Core::Player player{&library, &engine};
        QSignalSpy status{&player, &Core::Player::statusMessage};
        QStringList events;

        bool sawPrefix(const QString& prefix) const {
            for (const QList<QVariant>& arguments : status) {
                if (arguments.at(0).toString().startsWith(prefix)) {
                    return true;
                }
            }
            return false;
        }
        bool playing() const { return engine.state() == Audio::AudioEngine::State::Playing; }
    };

    // A stack that plays these ids from the server. Ids in `broken` answer their download-info
    // with `brokenFixture` instead. Null when there is no audio output.
    std::unique_ptr<Stack> makeStack(
        const QList<int>& ids,
        const QList<int>& broken = {},
        const QString& brokenFixture = {}
    ) {
        auto stack = std::make_unique<Stack>();
        if (!stack->engine.init().ok) {
            return nullptr;
        }
        stack->engine.setVolume(0);
        stack->api.setBaseUrl(server.baseUrl());
        stack->player.setNetworkRetryDelays(50, 200);
        QStringList good;
        QList<Yandex::Track> tracks;
        for (int id : ids) {
            Yandex::Track track;
            track.id = QString::number(id);
            track.title = QStringLiteral("t%1").arg(id);
            track.durationMs = 3000;
            tracks << track;
            if (broken.contains(id)) {
                server.fixture(
                    "GET", QStringLiteral("/tracks/%1/download-info").arg(id), brokenFixture
                );
            } else {
                good << track.id;
            }
        }
        server.audioTracks(good, mp3);
        Stack* raw = stack.get();
        stack->player.setQueue(
            tracks, "Q", false, {},
            [raw](Core::Player::TrackEvent event, const Yandex::Track& track, double) {
                raw->events << QStringLiteral("%1:%2").arg(static_cast<int>(event)).arg(track.id);
            }
        );
        return stack;
    }

    int requestsTo(const QString& path) const {
        int count = 0;
        for (const Tests::MockRequest& request : server.requests()) {
            count += request.path == path;
        }
        return count;
    }

private Q_SLOTS:
    void initTestCase() {
        QFile file(QStringLiteral(QIYAA_TEST_DATA "/sine440_3s.mp3"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        mp3 = file.readAll();
        // One /get-mp3/ handler for the suite (prefix routes never replace each other).
        server.onPrefix("GET", "/get-mp3/", [this](const Tests::MockRequest& request) {
            const QString id = request.path.section(u'/', -1).mid(1);  // ".../t<id>"
            return streamOverride.value(id, Tests::MockResponse{200, mp3});
        });
    }
    void init() { streamOverride.clear(); }

    void policyFollowsTheSpec_data() {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("failuresInRow");
        QTest::addColumn<bool>("hasNext");
        QTest::addColumn<bool>("endless");
        QTest::addColumn<int>("action");
        auto row = [](const char* name, FailureKind kind, int inRow, bool hasNext, bool endless,
                      FailureAction action) {
            QTest::newRow(name) << static_cast<int>(kind) << inRow << hasNext << endless
                                << static_cast<int>(action);
        };
        row("ERR-01 network", FailureKind::Network, 2, true, false, FailureAction::WaitForNetwork);
        row("ERR-04 first broken", FailureKind::Track, 0, true, false, FailureAction::Next);
        row("ERR-04 second broken", FailureKind::Track, 1, true, false, FailureAction::Next);
        row("ERR-05 third broken", FailureKind::Track, 2, true, false,
            FailureAction::StopAfterLimit);
        row("ERR-07 last of a finite queue", FailureKind::Track, 0, false, false,
            FailureAction::Stop);
        row("ERR-07 end of a wave", FailureKind::Track, 0, false, true, FailureAction::Next);
        row("ERR-08 account", FailureKind::Auth, 0, true, false, FailureAction::Stop);
    }
    void policyFollowsTheSpec() {
        QFETCH(int, kind);
        QFETCH(int, failuresInRow);
        QFETCH(bool, hasNext);
        QFETCH(bool, endless);
        QFETCH(int, action);
        QCOMPARE(
            static_cast<int>(Core::DecideOnFailure(
                static_cast<FailureKind>(kind), failuresInRow, hasNext, endless
            )),
            action
        );
    }

    void repliesAreClassifiedByKind() {
        using Kind = Yandex::RequestError::Kind;
        QNetworkAccessManager network;
        auto classify = [&](const QString& url) {
            QNetworkReply* reply = network.get(QNetworkRequest(QUrl(url)));
            QSignalSpy finished(reply, &QNetworkReply::finished);
            [&] { QVERIFY(finished.wait(5000)); }();
            const Yandex::RequestError error = Yandex::ClassifyReply(*reply);
            reply->deleteLater();
            return error;
        };
        QCOMPARE(classify(QStringLiteral("http://127.0.0.1:1/x")).kind, Kind::Network);
        server.json("GET", "/missing", "{}", 404);
        const Yandex::RequestError missing = classify(server.baseUrl() + "/missing");
        QCOMPARE(missing.kind, Kind::Http);
        QCOMPARE(missing.httpStatus, 404);
        QCOMPARE(Core::KindOf(missing), FailureKind::Track);
        server.json("GET", "/denied", "{}", 401);
        QCOMPARE(Core::KindOf(classify(server.baseUrl() + "/denied")), FailureKind::Auth);
        server.on("GET", "/cut", [](const Tests::MockRequest&) {
            return Tests::MockResponse{200, QByteArray(1000, 'x'), 0, 100};
        });
        QCOMPARE(classify(server.baseUrl() + "/cut").kind, Kind::Network);  // a body cut short
        server.json("GET", "/fine", "{}");
        QCOMPARE(classify(server.baseUrl() + "/fine").kind, Kind::None);
    }

    void brokenTrackIsSkipped() {  // ERR-04
        auto stack = makeStack({101, 102, 103}, {102}, "rotor-session-feedback/404-not-found");
        if (!stack) {
            QSKIP("no audio output");
        }
        stack->player.playIndex(1);
        QVERIFY(QTest::qWaitFor([&] { return stack->player.currentIndex() == 2; }, 5000));
        QVERIFY(QTest::qWaitFor([&] { return stack->playing(); }, 5000));
        QVERIFY(stack->sawPrefix(QStringLiteral("The track does not play: HTTP 404")));
        stack->player.stop();
    }

    void undecodableTrackIsSkipped() {  // ERR-04
        auto stack = makeStack({111, 112});
        if (!stack) {
            QSKIP("no audio output");
        }
        streamOverride.insert(QStringLiteral("111"), {200, QByteArray(20'000, 'x')});
        stack->player.playIndex(0);
        QVERIFY(QTest::qWaitFor([&] { return stack->player.currentIndex() == 1; }, 5000));
        QVERIFY(QTest::qWaitFor([&] { return stack->playing(); }, 5000));
        QVERIFY(stack->sawPrefix(QStringLiteral("The track does not play: ")));
        stack->player.stop();
    }

    void threeBrokenTracksInARowStop() {  // ERR-05
        auto stack = makeStack(
            {121, 122, 123, 124}, {121, 122, 123}, "rotor-session-feedback/404-not-found"
        );
        if (!stack) {
            QSKIP("no audio output");
        }
        stack->player.playIndex(0);
        QVERIFY(QTest::qWaitFor(
            [&] { return stack->sawPrefix(QStringLiteral("Stopped: 3 track(s) in a row")); }, 5000
        ));
        QCOMPARE(stack->player.currentIndex(), 2);
        QCOMPARE(stack->engine.state(), Audio::AudioEngine::State::Stopped);
        QCOMPARE(requestsTo("/tracks/124/download-info"), 0);
    }

    void aPlayingTrackResetsTheCount() {  // ERR-06
        auto stack = makeStack(
            {131, 132, 133, 134, 135}, {131, 133, 134}, "rotor-session-feedback/404-not-found"
        );
        if (!stack) {
            QSKIP("no audio output");
        }
        stack->player.playIndex(0);  // broken (1), then 132 plays: the count is back to 0
        QVERIFY(QTest::qWaitFor(
            [&] { return stack->player.currentIndex() == 1 && stack->playing(); }, 5000
        ));
        stack->player.next();  // 133 and 134 broken (1, 2): 135 plays
        QVERIFY(QTest::qWaitFor(
            [&] { return stack->player.currentIndex() == 4 && stack->playing(); }, 5000
        ));
        QVERIFY(!stack->sawPrefix(QStringLiteral("Stopped")));
        stack->player.stop();
    }

    void brokenLastTrackStops() {  // ERR-07
        auto stack = makeStack({141}, {141}, "rotor-session-feedback/503-string-error");
        if (!stack) {
            QSKIP("no audio output");
        }
        stack->player.playIndex(0);
        QVERIFY(QTest::qWaitFor(
            [&] { return stack->sawPrefix(QStringLiteral("The track does not play: HTTP 503")); },
            5000
        ));
        QCOMPARE(stack->player.currentIndex(), 0);
        QCOMPARE(stack->engine.state(), Audio::AudioEngine::State::Stopped);
    }

    void rejectedAccountStopsWithoutCounting() {  // ERR-08
        auto stack = makeStack({151, 152}, {151}, "users-likes-artists/401-session-expired");
        if (!stack) {
            QSKIP("no audio output");
        }
        stack->player.playIndex(0);
        QVERIFY(QTest::qWaitFor(
            [&] { return stack->sawPrefix(QStringLiteral("Access error: HTTP 401")); }, 5000
        ));
        QCOMPARE(stack->player.currentIndex(), 0);
        QCOMPARE(stack->engine.state(), Audio::AudioEngine::State::Stopped);
        QCOMPARE(requestsTo("/tracks/152/download-info"), 0);
    }

    void noNetworkPausesAndContinuesTheTrack() {  // ERR-01, ERR-02, ERR-09
        auto stack = makeStack({161, 162});
        if (!stack) {
            QSKIP("no audio output");
        }
        const int playReports = requestsTo("/play-audio");
        stack->api.setBaseUrl(QStringLiteral("http://127.0.0.1:1"));  // refused: no network
        stack->player.setNetworkOnline(false);
        stack->player.playIndex(0);
        QVERIFY(QTest::qWaitFor([&] { return stack->player.isWaitingForNetwork(); }, 5000));
        QVERIFY(stack->sawPrefix(QStringLiteral("No network")));
        QCOMPARE(stack->engine.state(), Audio::AudioEngine::State::Paused);
        QCOMPARE(stack->player.currentIndex(), 0);
        QTest::qWait(300);  // offline: no retries, and it stays on the track
        QCOMPARE(stack->player.currentIndex(), 0);

        stack->api.setBaseUrl(server.baseUrl());
        stack->player.setNetworkOnline(true);  // back: tries at once
        QVERIFY(QTest::qWaitFor([&] { return stack->playing(); }, 5000));
        QCOMPARE(stack->player.currentIndex(), 0);
        QVERIFY(!stack->player.isWaitingForNetwork());
        QCOMPARE(requestsTo("/play-audio"), playReports + 1);  // one start, after the wait
        stack->player.stop();
    }

    void retriesWithoutBeingToldAndKeepsAPause() {  // ERR-02
        auto stack = makeStack({171});
        if (!stack) {
            QSKIP("no audio output");
        }
        stack->api.setBaseUrl(QStringLiteral("http://127.0.0.1:1"));
        stack->player.playIndex(0);
        QVERIFY(QTest::qWaitFor([&] { return stack->player.isWaitingForNetwork(); }, 5000));
        stack->player.pause();  // the user pauses while waiting
        stack->api.setBaseUrl(server.baseUrl());  // the retry timer finds the network back
        QVERIFY(QTest::qWaitFor([&] { return !stack->player.isWaitingForNetwork(); }, 5000));
        QTest::qWait(200);
        QCOMPARE(stack->engine.state(), Audio::AudioEngine::State::Paused);
        stack->player.stop();
    }

    void brokenDownloadWaitsInsteadOfMovingOn() {  // ERR-03, ERR-09
        auto stack = makeStack({181, 182});
        if (!stack) {
            QSKIP("no audio output");
        }
        Tests::MockResponse cut{200, mp3};
        cut.truncateAfter = 9'000;  // a bit over 1 s of the 3 s track
        streamOverride.insert(QStringLiteral("181"), cut);
        const int playReports = requestsTo("/play-audio");
        stack->player.playIndex(0);
        QVERIFY(QTest::qWaitFor([&] { return stack->player.isWaitingForNetwork(); }, 8000));
        QCOMPARE(stack->player.currentIndex(), 0);
        const double kept = stack->engine.positionSeconds();
        QVERIFY2(kept > 0.5, qPrintable(QString::number(kept)));
        QCOMPARE(stack->events, QStringList{"0:181"});  // still open: no skip

        streamOverride.remove(QStringLiteral("181"));  // the network is back
        QVERIFY(QTest::qWaitFor([&] { return stack->playing(); }, 5000));
        QCOMPARE(stack->player.currentIndex(), 0);
        QVERIFY(stack->engine.positionSeconds() >= kept - 0.1);
        QCOMPARE(requestsTo("/play-audio"), playReports + 1);
        QCOMPARE(stack->events, QStringList{"0:181"});
        stack->player.stop();
    }
};

QTEST_GUILESS_MAIN(TestFailures)
#include "failures_test.moc"
