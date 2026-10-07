#include "audio/audio_engine.h"
#include "core/jam_mode.h"
#include "core/player.h"
#include "jam/codec.h"
#include "jam/host_session.h"
#include "jam/session_store.h"
#include "support/jam_stub_server.h"
#include "support/mock_http_server.h"
#include "support/spec_fixtures.h"
#include "yandex/api_client.h"
#include "yandex/library.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>

#include <optional>
#include <vector>

// Jam::HostSession against spec/jam/host.md, by scenario ID: the session talks to a stub server on
// localhost, the queue is a real Player in jam mode, the catalog a MockHttpServer.

namespace {

const QString kRoom = QStringLiteral("7k3m9q2x");
const QString kHostSecret = QStringLiteral("R0CuY0ewFywBJU_1W65a_1GZ9ERuf21kPUAYWz9HUUU");
const QString kJoinUrl =
    QStringLiteral("https://jam.example.org/j/7k3m9q2x#WDkyFgMr5iV3hKwManPvsg");
const QString kHostId = QStringLiteral("h7k2m9");
const QString kGuestId = QStringLiteral("a4n8q1");

QJsonObject Example(const QString& name) {
    return Tests::SpecObject(QStringLiteral("jam/protocol/examples/%1.json").arg(name));
}

QByteArray Text(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray Created(const QString& id) {
    QJsonObject message = Example(QStringLiteral("created/ok"));
    message[QStringLiteral("id")] = id;
    return Text(message);
}

QByteArray Resumed(const QString& id, bool restored) {
    return Text(
        {{QStringLiteral("type"), QStringLiteral("resumed")},
         {QStringLiteral("id"), id},
         {QStringLiteral("restored"), restored}}
    );
}

QByteArray Rejected(const QString& id, const QString& reason) {
    return Text(
        {{QStringLiteral("type"), QStringLiteral("rejected")},
         {QStringLiteral("id"), id},
         {QStringLiteral("reason"), reason}}
    );
}

QByteArray Ack(const QString& id) {
    return Text({{QStringLiteral("type"), QStringLiteral("ack")}, {QStringLiteral("id"), id}});
}

QByteArray Ended(const QString& reason) {
    return Text(
        {{QStringLiteral("type"), QStringLiteral("ended")}, {QStringLiteral("reason"), reason}}
    );
}

QJsonObject TrackJson(const QString& id) {
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("albumId"), QStringLiteral("1")},
        {QStringLiteral("title"), QStringLiteral("T") + id},
        {QStringLiteral("artists"), QJsonArray{QStringLiteral("A")}},
        {QStringLiteral("durationMs"), 3'000}
    };
}

QJsonObject Item(
    const QString& itemId,
    const QString& trackId,
    const QString& addedBy = kGuestId,
    bool pinned = false
) {
    return {
        {QStringLiteral("itemId"), itemId},
        {QStringLiteral("track"), TrackJson(trackId)},
        {QStringLiteral("addedBy"), addedBy},
        {QStringLiteral("addedAt"), static_cast<qint64>(1'790'730'000'000)},
        {QStringLiteral("pinned"), pinned}
    };
}

QByteArray StateOf(qint64 version, const QJsonArray& queue) {
    QJsonObject message = Example(QStringLiteral("state/host"));
    message[QStringLiteral("version")] = version;
    QJsonObject room = message[QStringLiteral("room")].toObject();
    room[QStringLiteral("queue")] = queue;
    room[QStringLiteral("fallback")] =
        QJsonObject{{QStringLiteral("seeds"), QJsonArray{}}, {QStringLiteral("seedsVersion"), 0}};
    message[QStringLiteral("room")] = room;
    return Text(message);
}

QJsonObject YandexTrack(const QString& id, bool available = true) {
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), QStringLiteral("T") + id},
        {QStringLiteral("artists"),
         QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Кино")}}}},
        {QStringLiteral("albums"),
         QJsonArray{QJsonObject{
             {QStringLiteral("id"), 7},
             {QStringLiteral("coverUri"), QStringLiteral("avatars.yandex.net/a/%%")}
         }}},
        {QStringLiteral("durationMs"), 200'000},
        {QStringLiteral("available"), available}
    };
}

Jam::Session StoredSession(QStringList outbox = {}) {
    return Jam::Session{kRoom, kHostSecret, kJoinUrl, std::nullopt, std::move(outbox)};
}

QStringList Ids(const Core::Player& player) {
    QStringList ids;
    for (const Yandex::Track& track : player.playlist()) {
        ids << track.id;
    }
    return ids;
}

template <typename T>
std::vector<T> OfType(const std::vector<Jam::ClientMessage>& messages) {
    std::vector<T> found;
    for (const Jam::ClientMessage& message : messages) {
        if (const T* typed = std::get_if<T>(&message)) {
            found.push_back(*typed);
        }
    }
    return found;
}

}  // namespace

class TestJamHost : public QObject {
    Q_OBJECT
private:
    Tests::MockHttpServer catalog;
    Tests::LocalNetworkAccessManager networkManager;
    Yandex::ApiClient api{&networkManager};
    Yandex::Library library{&api};

    struct Stack {
        Tests::JamStubServer server;
        QTemporaryDir directory;
        Jam::SessionStore store{directory.filePath(QStringLiteral("jam-session.json"))};
        QString serverUrl = server.serverUrl();
        int ids = 0;
        int greeted = 0;
        bool share = false;
        Audio::AudioEngine engine;  // not initialised: no sound card
        Core::Player player;
        Core::JamMode jam;
        Jam::HostSession host;
        QSignalSpy refused;
        QSignalSpy ended;

        explicit Stack(Yandex::Library* library, const std::optional<Jam::Session>& stored = {})
            : player(library, &engine)
            , jam(&player, library)
            , host(&jam, library, Stored(store, stored), options())
            , refused(&host, &Jam::HostSession::refused)
            , ended(&host, &Jam::HostSession::ended) { }

        static Jam::SessionStore
        Stored(const Jam::SessionStore& store, const std::optional<Jam::Session>& stored) {
            if (stored) {
                store.save(*stored);
            }
            return store;
        }

        Jam::HostOptions options() {
            Jam::HostOptions options;
            options.client.appVersion = QStringLiteral("test");
            options.client.reconnectDelaysMs = {50, 100, 200};
            options.config = [this] { return Jam::HostConfig{serverUrl, false, share}; };
            options.queueTitle = QStringLiteral("Джем");
            options.newId = [this] { return QStringLiteral("r%1").arg(++ids); };
            options.resumeRetryMs = 500;
            return options;
        }

        // Waits for the next connection and greets it.
        bool open() {
            if (!QTest::qWaitFor([&] { return server.count() > greeted; }, 5000)
                || !QTest::qWaitFor([&] { return !server.received().isEmpty(); }, 5000)) {
                return false;  // no hello
            }
            greeted = server.count();
            server.welcome();
            return true;
        }

        std::vector<Jam::ClientMessage> sent() const {
            std::vector<Jam::ClientMessage> messages = server.messages();
            std::erase_if(messages, [](const Jam::ClientMessage& message) {
                return std::holds_alternative<Jam::Hello>(message);
            });
            return messages;
        }

        bool waitSent(qsizetype count) {
            return QTest::qWaitFor(
                [&] { return static_cast<qsizetype>(sent().size()) >= count; }, 5000
            );
        }

        void receive(const QByteArray& message) {
            server.send(message);
            QTest::qWait(30);
        }

        // A jam created on the server: "r1" is the create.
        bool created() {
            if (!host.create(QStringLiteral("Маша")) || !open() || !waitSent(1)) {
                return false;
            }
            server.send(Created(QStringLiteral("r1")));
            return QTest::qWaitFor([&] { return host.isConnected(); }, 5000);
        }

        void state(qint64 version, const QJsonArray& queue) {
            server.send(StateOf(version, queue));
            QTest::qWait(30);
        }

        Jam::HostEnd lastEnd() const { return ended.last().at(0).value<Jam::HostEnd>(); }
    };

private Q_SLOTS:
    void initTestCase() {
        qRegisterMetaType<Core::JamPlayback>();
        qRegisterMetaType<Jam::HostEnd>();
        api.setBaseUrl(catalog.baseUrl());
        api.setToken(QStringLiteral("test-token"));
        catalog.fixture("GET", "/account/status", "account-status/ok");
        catalog.fixture("POST", "/rotor/session/new", "rotor-session-new/all-unavailable");
        QFile file(QStringLiteral(QIYAA_TEST_DATA "/sine440_3s.mp3"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        catalog.audioTracks(
            {QStringLiteral("11"), QStringLiteral("22"), QStringLiteral("33"), QStringLiteral("44"),
             QStringLiteral("55")},
            file.readAll()
        );
        bool done = false;
        library.connectAccount([&](const Yandex::Account&, const QString&) { done = true; });
        QVERIFY(QTest::qWaitFor([&] { return done; }, 5000));
    }

    void createGoesOutAfterWelcomeAndCreatedStartsTheJam() {
        Stack stack(&library);
        QVERIFY(stack.host.create(QStringLiteral("  Маша  ")));
        QCOMPARE(stack.host.phase(), Jam::HostPhase::Creating);
        QVERIFY(stack.open());
        QVERIFY(stack.waitSent(1));
        QCOMPARE(stack.sent().front(), Jam::ClientMessage(Jam::Create{"r1", "Маша", std::nullopt}));
        QCOMPARE(stack.server.last().requestUrl().path(), QStringLiteral("/ws"));
        stack.server.send(Created(QStringLiteral("r1")));
        QTRY_VERIFY(stack.host.isConnected());
        QCOMPARE(stack.store.load(), std::optional(StoredSession()));
        QCOMPARE(stack.host.phase(), Jam::HostPhase::Active);
        QCOMPARE(stack.host.joinUrl(), kJoinUrl);
        QVERIFY(stack.jam.isActive());
        QCOMPARE(stack.player.queueTitle(), QStringLiteral("Джем"));
        QVERIFY(stack.waitSent(2));
        QCOMPARE(
            stack.sent().back(),
            Jam::ClientMessage(Jam::Playing{Jam::Source::Idle, {}, {}, 0, true})
        );
    }

    void createRefusesAMissingServerAndABlankName() {
        Stack stack(&library);
        QVERIFY(!stack.host.create(QStringLiteral("   ")));
        stack.serverUrl.clear();
        QVERIFY(!stack.host.create(QStringLiteral("Маша")));
        QTest::qWait(100);
        QCOMPARE(stack.server.count(), 0);
        QCOMPARE(stack.host.phase(), Jam::HostPhase::None);
    }

    void aRefusedCreateReportsTheReasonAndLeavesNoJam() {  // ROOM-02
        Stack stack(&library);
        stack.host.create(QStringLiteral("Маша"));
        QVERIFY(stack.open());
        QVERIFY(stack.waitSent(1));
        stack.receive(Rejected(QStringLiteral("r1"), QStringLiteral("rate-limited")));
        QTRY_COMPARE(stack.refused.size(), 1);
        QCOMPARE(stack.refused.at(0).at(0).toString(), QStringLiteral("rate-limited"));
        QCOMPARE(stack.host.phase(), Jam::HostPhase::None);
        QCOMPARE(stack.host.connection(), Jam::Status::Stopped);
        QVERIFY(!stack.jam.isActive());
        QVERIFY(!stack.store.load());
    }

    void statesFeedTheQueueAndAnOlderVersionIsIgnored() {  // HOST-01
        Stack stack(&library);
        QVERIFY(stack.created());
        stack.state(5, {Item("i1", "11")});
        QTRY_COMPARE(Ids(stack.player), QStringList{QStringLiteral("11")});
        stack.state(4, {Item("i1", "11"), Item("i2", "22")});
        QCOMPARE(Ids(stack.player), QStringList{QStringLiteral("11")});
        stack.state(6, {Item("i1", "11"), Item("i2", "22")});
        QTRY_COMPARE(Ids(stack.player), (QStringList{"11", "22"}));
        QCOMPARE(stack.host.room()->queue.size(), static_cast<size_t>(2));

        // A new connection takes its first state whatever the version.
        stack.server.last().close();
        QVERIFY(stack.open());
        QTRY_VERIFY(!OfType<Jam::Resume>(stack.sent()).empty());
        stack.receive(Resumed(QStringLiteral("r2"), true));
        QTRY_VERIFY(stack.host.isConnected());
        stack.state(2, {Item("i2", "22"), Item("i3", "33")});
        QTRY_COMPARE(Ids(stack.player), (QStringList{"11", "22", "33"}));
    }

    void startedWaitsInTheStoredOutboxOfflineAndGoesWithResume() {  // HOST-05, HOST-25, HOST-26
        Stack stack(&library);
        QVERIFY(stack.created());
        // i3 keeps the player from waiting for the jam wave after i2: with no sound card a waiting
        // player reports idle.
        stack.state(1, {Item("i1", "11"), Item("i2", "22"), Item("i3", "33")});
        QTRY_VERIFY(OfType<Jam::Started>(stack.sent()) == std::vector{Jam::Started{"i1"}});

        stack.server.last().close();
        QTRY_VERIFY(!stack.host.isConnected());
        stack.player.next();
        QTRY_COMPARE(stack.store.load()->outbox, QStringList{QStringLiteral("i2")});
        QVERIFY(!stack.host.pin(QStringLiteral("i2")));

        QVERIFY(stack.open());
        QVERIFY(stack.waitSent(1));
        QCOMPARE(
            stack.sent().front(),
            Jam::ClientMessage(Jam::Resume{"r2", kRoom, kHostSecret, std::nullopt, {"i2"}})
        );
        stack.receive(Resumed(QStringLiteral("r2"), false));
        QTRY_VERIFY(stack.host.isConnected());
        QCOMPARE(stack.store.load()->outbox, QStringList());
        QVERIFY(stack.waitSent(2));
        const Jam::ClientMessage last = stack.sent().back();
        const auto* playing = std::get_if<Jam::Playing>(&last);
        QVERIFY2(
            playing && playing->source == Jam::Source::Item && playing->itemId == "i2",
            Jam::Encode(last).constData()
        );
        QVERIFY(stack.host.pin(QStringLiteral("i2")));
    }

    void theLatestSnapshotAndANewLinkAreStored() {  // HOST-22
        Stack stack(&library);
        QVERIFY(stack.created());
        const QJsonObject snapshot = Example(QStringLiteral("snapshot/ok"));
        stack.receive(Text(snapshot));
        QTRY_COMPARE(
            stack.store.load()->snapshot, std::optional(snapshot[QStringLiteral("data")].toObject())
        );
        const QString url =
            QStringLiteral("https://jam.example.org/j/7k3m9q2x#AAAAAAAAAAAAAAAAAAAAAA");
        stack.receive(Text(
            {{QStringLiteral("type"), QStringLiteral("linkRotated")},
             {QStringLiteral("id"), QStringLiteral("r9")},
             {QStringLiteral("joinSecret"), QStringLiteral("AAAAAAAAAAAAAAAAAAAAAA")},
             {QStringLiteral("joinUrl"), url}}
        ));
        QTRY_COMPARE(stack.store.load()->joinUrl, url);
        QCOMPARE(stack.host.joinUrl(), url);
    }

    void continueResumesWithTheStoredSnapshotAndOutbox() {  // HOST-23
        Jam::Session stored = StoredSession({QStringLiteral("i7")});
        stored.snapshot = Example(QStringLiteral("snapshot/ok"))[QStringLiteral("data")].toObject();
        Stack stack(&library, stored);
        QVERIFY(stack.host.hasStoredSession());
        stack.host.continueStored();
        QVERIFY(stack.jam.isActive());
        QVERIFY(stack.player.playlist().isEmpty());  // the desktop does not keep the queue
        QVERIFY(!stack.host.hasStoredSession());
        QCOMPARE(stack.host.phase(), Jam::HostPhase::Active);
        QVERIFY(stack.open());
        QVERIFY(stack.waitSent(1));
        QCOMPARE(
            stack.sent().front(),
            Jam::ClientMessage(Jam::Resume{"r1", kRoom, kHostSecret, stored.snapshot, {"i7"}})
        );
        stack.receive(Resumed(QStringLiteral("r1"), true));
        QTRY_VERIFY(stack.host.isConnected());
        QCOMPARE(stack.store.load()->outbox, QStringList());
    }

    void noClearsTheStorageThenResumesTheRoomOnlyToEndIt() {  // HOST-23
        Stack stack(&library, StoredSession());
        stack.host.discardStored();
        QVERIFY(!stack.store.load());
        QVERIFY(!stack.jam.isActive());
        QVERIFY(stack.open());
        QVERIFY(stack.waitSent(1));
        QCOMPARE(
            stack.sent().front(),
            Jam::ClientMessage(Jam::Resume{"r1", kRoom, kHostSecret, std::nullopt, {}})
        );
        stack.receive(Resumed(QStringLiteral("r1"), false));
        QVERIFY(stack.waitSent(2));
        QCOMPARE(stack.sent().back(), Jam::ClientMessage(Jam::End{"r2"}));
        stack.receive(Ack(QStringLiteral("r2")));
        QTRY_COMPARE(stack.host.connection(), Jam::Status::Stopped);
        QCOMPARE(stack.host.phase(), Jam::HostPhase::None);
        QCOMPARE(stack.ended.size(), 0);
    }

    void aResumeRefusedForAGoneRoomEndsTheJam() {  // HOST-24
        Stack stack(&library, StoredSession());
        stack.host.continueStored();
        QVERIFY(stack.open());
        QVERIFY(stack.waitSent(1));
        stack.receive(Rejected(QStringLiteral("r1"), QStringLiteral("room-not-found")));
        QTRY_COMPARE(stack.ended.size(), 1);
        QCOMPARE(stack.lastEnd(), Jam::HostEnd::Gone);
        QCOMPARE(stack.refused.size(), 0);
        QVERIFY(!stack.store.load());
        QVERIFY(!stack.jam.isActive());
        QCOMPARE(stack.host.phase(), Jam::HostPhase::None);
    }

    void aResumeRefusedForAnotherReasonIsTriedAgainLater() {
        Stack stack(&library, StoredSession());
        stack.host.continueStored();
        QVERIFY(stack.open());
        QVERIFY(stack.waitSent(1));
        stack.receive(Rejected(QStringLiteral("r1"), QStringLiteral("server-full")));
        QTRY_COMPARE(stack.refused.size(), 1);
        QCOMPARE(stack.refused.at(0).at(0).toString(), QStringLiteral("server-full"));
        QCOMPARE(stack.server.count(), 1);
        QVERIFY(stack.open());
        QVERIFY(stack.waitSent(1));
        QVERIFY(std::holds_alternative<Jam::Resume>(stack.sent().front()));
        QVERIFY(stack.jam.isActive());
        QCOMPARE(stack.ended.size(), 0);
    }

    void aGuestsSearchAnswersWithUpTo20AvailableTracksOrTheError() {  // HOST-28, HOST-29
        Stack stack(&library);
        QVERIFY(stack.created());
        QJsonArray results;
        for (int i = 1; i <= 30; ++i) {
            results << YandexTrack(QString::number(i), i != 2);
        }
        const QJsonObject found{
            {QStringLiteral("tracks"), QJsonObject{{QStringLiteral("results"), results}}}
        };
        catalog.result("GET", "/search", Text(found));
        stack.receive(Text(
            {{QStringLiteral("type"), QStringLiteral("searchRequest")},
             {QStringLiteral("requestId"), QStringLiteral("q1")},
             {QStringLiteral("text"), QStringLiteral("кино")}}
        ));
        QTRY_VERIFY(!OfType<Jam::SearchResult>(stack.sent()).empty());
        QCOMPARE(
            catalog.last(QStringLiteral("/search"))->query.queryItemValue(QStringLiteral("type")),
            QStringLiteral("track")
        );
        QCOMPARE(
            catalog.last(QStringLiteral("/search"))->query.queryItemValue(QStringLiteral("text")),
            QStringLiteral("кино")
        );
        const Jam::SearchResult reply = OfType<Jam::SearchResult>(stack.sent()).back();
        QCOMPARE(reply.requestId, QStringLiteral("q1"));
        QVERIFY(reply.tracks);
        QStringList ids;
        for (const Jam::Track& track : *reply.tracks) {
            ids << track.id;
        }
        QStringList expected;
        for (int i = 1; i <= 21; ++i) {
            if (i != 2) {
                expected << QString::number(i);
            }
        }
        QCOMPARE(ids, expected);
        QCOMPARE(reply.tracks->front().coverUri, QStringLiteral("avatars.yandex.net/a/%%"));

        catalog.json("GET", "/search", "{}", 500);
        stack.receive(Text(
            {{QStringLiteral("type"), QStringLiteral("searchRequest")},
             {QStringLiteral("requestId"), QStringLiteral("q2")},
             {QStringLiteral("text"), QStringLiteral("кино")}}
        ));
        QTRY_COMPARE(OfType<Jam::SearchResult>(stack.sent()).size(), static_cast<size_t>(2));
        QCOMPARE(
            OfType<Jam::SearchResult>(stack.sent()).back(),
            (Jam::SearchResult{"q2", std::nullopt, Jam::SearchError::Failed})
        );

        catalog.fixture("GET", "/search", "users-likes-artists/401-session-expired");
        stack.receive(Text(
            {{QStringLiteral("type"), QStringLiteral("searchRequest")},
             {QStringLiteral("requestId"), QStringLiteral("q3")},
             {QStringLiteral("text"), QStringLiteral("кино")}}
        ));
        QTRY_COMPARE(OfType<Jam::SearchResult>(stack.sent()).size(), static_cast<size_t>(3));
        QCOMPARE(
            OfType<Jam::SearchResult>(stack.sent()).back(),
            (Jam::SearchResult{"q3", std::nullopt, Jam::SearchError::Unauthorized})
        );
    }

    void aCheckAnswersForEveryIdInOrderAndAFailureFailsThemAll() {  // HOST-30
        Stack stack(&library);
        QVERIFY(stack.created());
        catalog.result(
            "POST", "/tracks/",
            QJsonDocument(QJsonArray{
                              YandexTrack(QStringLiteral("22"), false),
                              YandexTrack(QStringLiteral("11"))
                          })
                .toJson(QJsonDocument::Compact)
        );
        stack.receive(Text(
            {{QStringLiteral("type"), QStringLiteral("validateRequest")},
             {QStringLiteral("requestId"), QStringLiteral("v1")},
             {QStringLiteral("trackIds"),
              QJsonArray{QStringLiteral("11"), QStringLiteral("22"), QStringLiteral("33")}}}
        ));
        QTRY_VERIFY(!OfType<Jam::ValidateResult>(stack.sent()).empty());
        Jam::Track first{"11", "7", "T11", {"Кино"}, 200'000, "avatars.yandex.net/a/%%"};
        QCOMPARE(
            OfType<Jam::ValidateResult>(stack.sent()).back(),
            (Jam::ValidateResult{
                "v1",
                {Jam::ValidateEntry{"11", first, std::nullopt},
                 Jam::ValidateEntry{"22", std::nullopt, Jam::ValidateReason::TrackUnavailable},
                 Jam::ValidateEntry{"33", std::nullopt, Jam::ValidateReason::TrackUnavailable}}
            })
        );

        catalog.json("POST", "/tracks/", "{}", 500);
        stack.receive(Text(
            {{QStringLiteral("type"), QStringLiteral("validateRequest")},
             {QStringLiteral("requestId"), QStringLiteral("v2")},
             {QStringLiteral("trackIds"), QJsonArray{QStringLiteral("11"), QStringLiteral("22")}}}
        ));
        QTRY_COMPARE(OfType<Jam::ValidateResult>(stack.sent()).size(), static_cast<size_t>(2));
        QCOMPARE(
            OfType<Jam::ValidateResult>(stack.sent()).back(),
            (Jam::ValidateResult{
                "v2",
                {Jam::ValidateEntry{"11", std::nullopt, Jam::ValidateReason::Failed},
                 Jam::ValidateEntry{"22", std::nullopt, Jam::ValidateReason::Failed}}
            })
        );
    }

    void aSkipCommandMovesOnOnlyForTheCurrentItem() {  // HOST-18, HOST-19
        Stack stack(&library);
        QVERIFY(stack.created());
        stack.state(1, {Item("i1", "11"), Item("i2", "22")});
        QTRY_COMPARE(stack.player.currentIndex(), 0);
        stack.receive(Text(
            {{QStringLiteral("type"), QStringLiteral("command")},
             {QStringLiteral("kind"), QStringLiteral("skip")},
             {QStringLiteral("itemId"), QStringLiteral("i2")}}
        ));
        QCOMPARE(stack.player.currentIndex(), 0);
        stack.receive(Text(
            {{QStringLiteral("type"), QStringLiteral("command")},
             {QStringLiteral("kind"), QStringLiteral("skip")},
             {QStringLiteral("itemId"), QStringLiteral("i1")}}
        ));
        QTRY_COMPARE(stack.player.currentIndex(), 1);
    }

    void theHostEndsTheJamAndItsItemsStay() {  // HOST-32
        Stack stack(&library);
        QVERIFY(stack.created());
        stack.state(1, {Item("i1", "11"), Item("i2", "22")});
        QTRY_COMPARE(Ids(stack.player), (QStringList{"11", "22"}));
        stack.host.end();
        QTRY_VERIFY(!OfType<Jam::End>(stack.sent()).empty());
        QTRY_COMPARE(stack.server.last().state(), QAbstractSocket::UnconnectedState);
        QVERIFY(!stack.store.load());
        QVERIFY(!stack.jam.isActive());
        QCOMPARE(Ids(stack.player), (QStringList{"11", "22"}));
        QCOMPARE(stack.ended.size(), 1);
        QCOMPARE(stack.lastEnd(), Jam::HostEnd::ByHost);
        QCOMPARE(stack.host.phase(), Jam::HostPhase::None);
    }

    void endedFromTheServerEndsTheJamHereWithoutAnEnd() {  // HOST-32
        Stack stack(&library);
        QVERIFY(stack.created());
        stack.receive(Ended(QStringLiteral("expired")));
        QTRY_COMPARE(stack.ended.size(), 1);
        QCOMPARE(stack.lastEnd(), Jam::HostEnd::Expired);
        QVERIFY(OfType<Jam::End>(stack.sent()).empty());
        QVERIFY(!stack.store.load());
        QVERIFY(!stack.jam.isActive());
    }

    void theHostAddsAndPlaysNextAndThePinWaitsForTheItem() {  // HOST-20
        Stack stack(&library);
        QVERIFY(stack.created());
        Yandex::Track track;
        track.id = QStringLiteral("44");
        track.title = QStringLiteral("D");
        track.artists = {QStringLiteral("Сплин")};
        QVERIFY(stack.host.add(track));
        QTRY_VERIFY(!OfType<Jam::Add>(stack.sent()).empty());
        QCOMPARE(
            OfType<Jam::Add>(stack.sent()).back(), (Jam::Add{"r2", {}, Jam::JamTrackOf(track)})
        );
        Yandex::Track next;
        next.id = QStringLiteral("55");
        next.title = QStringLiteral("E");
        QVERIFY(stack.host.playNext(next));
        QTRY_COMPARE(OfType<Jam::Add>(stack.sent()).size(), static_cast<size_t>(2));
        QVERIFY(OfType<Jam::Pin>(stack.sent()).empty());
        stack.state(1, {Item("i1", "11"), Item("i2", "44", kHostId), Item("i3", "55", kHostId)});
        QTRY_VERIFY(!OfType<Jam::Pin>(stack.sent()).empty());
        QCOMPARE(OfType<Jam::Pin>(stack.sent()).back(), (Jam::Pin{"r4", "i3"}));
        stack.state(2, {Item("i1", "11"), Item("i2", "44", kHostId), Item("i3", "55", kHostId)});
        QCOMPARE(OfType<Jam::Pin>(stack.sent()).size(), static_cast<size_t>(1));
        QVERIFY(stack.host.playNext(track));
        QTRY_COMPARE(OfType<Jam::Pin>(stack.sent()).size(), static_cast<size_t>(2));
        QCOMPARE(OfType<Jam::Pin>(stack.sent()).back(), (Jam::Pin{"r5", "i2"}));
        QVERIFY(stack.host.remove(QStringLiteral("i1")));
        QVERIFY(stack.host.kick(kGuestId));
        QVERIFY(stack.host.rotateLink());
        QVERIFY(stack.host.changeSettings(Jam::SettingsPatch{Jam::Order::Fifo, {}, {}, {}}));
        QTRY_VERIFY(!OfType<Jam::ChangeSettings>(stack.sent()).empty());
    }

    void listeningAlongSendsTheFilesOnlyWhenTheHostSharesThem() {  // LISTEN-01 to LISTEN-05
        Stack stack(&library);
        QVERIFY(stack.created());
        const QUrl file(QStringLiteral("https://s1.storage.yandex.net/get-mp3/"
                                       "0123456789abcdef0123456789abcdef/65cd937b03427/t.mp3"));
        Core::JamPlayback playback;
        playback.kind = Core::JamPlayback::Kind::Item;
        playback.itemId = QStringLiteral("i1");
        playback.paused = false;
        playback.link = file;
        const QUrl next(
            QString(file.toString()).replace(QStringLiteral("t.mp3"), QStringLiteral("n.mp3"))
        );
        playback.nextLink = next;
        const auto lastPlaying = [&] { return OfType<Jam::Playing>(stack.sent()).back(); };
        Q_EMIT stack.jam.playback(playback);
        QTRY_COMPARE(OfType<Jam::Playing>(stack.sent()).size(), static_cast<size_t>(2));
        QCOMPARE(lastPlaying().listenUrl, QString());
        QCOMPARE(lastPlaying().listenNextUrl, QString());
        stack.share = true;
        Q_EMIT stack.jam.playback(playback);
        QTRY_COMPARE(OfType<Jam::Playing>(stack.sent()).size(), static_cast<size_t>(3));
        QCOMPARE(lastPlaying().listenUrl, file.toString());
        QCOMPARE(lastPlaying().listenNextUrl, next.toString());
        playback.link = QUrl(QStringLiteral("http://127.0.0.1:8080/get-mp3/a/b/t1"));
        playback.nextLink = QUrl();
        Q_EMIT stack.jam.playback(playback);  // not Yandex's: the server would refuse it
        QTRY_COMPARE(OfType<Jam::Playing>(stack.sent()).size(), static_cast<size_t>(4));
        QCOMPARE(lastPlaying().listenUrl, QString());
        QCOMPARE(lastPlaying().listenNextUrl, QString());
        QCOMPARE(lastPlaying().itemId, QStringLiteral("i1"));
        playback = Core::JamPlayback{};  // idle: never a link
        playback.link = file;
        Q_EMIT stack.jam.playback(playback);
        QTRY_COMPARE(OfType<Jam::Playing>(stack.sent()).size(), static_cast<size_t>(5));
        QCOMPARE(lastPlaying().source, Jam::Source::Idle);
        QCOMPARE(lastPlaying().listenUrl, QString());
    }

    void aRefusedPlayNextTellsTheReason() {  // HOST-20
        Stack stack(&library);
        QVERIFY(stack.created());
        Yandex::Track track;
        track.id = QStringLiteral("44");
        track.title = QStringLiteral("D");
        QVERIFY(stack.host.playNext(track));
        QTRY_VERIFY(!OfType<Jam::Add>(stack.sent()).empty());
        stack.receive(Rejected(QStringLiteral("r2"), QStringLiteral("queue-limit")));
        QTRY_COMPARE(stack.refused.size(), 1);
        QCOMPARE(stack.refused.at(0).at(0).toString(), QStringLiteral("queue-limit"));
        stack.state(1, {Item("i2", "44", kHostId)});
        QTest::qWait(50);
        QVERIFY(OfType<Jam::Pin>(stack.sent()).empty());
    }

    void aJamTrackKeepsOnlyWhatTheProtocolTakes() {
        Yandex::Track track;
        track.id = QStringLiteral("38633712");
        track.albumId = QStringLiteral("not an id");
        track.title = QStringLiteral(" Группа\tкрови ") + QString(200, u'x');
        for (int i = 0; i < 12; ++i) {
            track.artists << (i == 3 ? QStringLiteral("  ") : QStringLiteral("A%1").arg(i));
        }
        track.durationMs = -5;
        track.coverUri = QStringLiteral("avatars.yandex.net/get-music-content/95061/%%");
        const std::optional<Jam::Track> jamTrack = Jam::JamTrackOf(track);
        QVERIFY(jamTrack);
        QCOMPARE(jamTrack->albumId, QString());
        QCOMPARE(jamTrack->title.size(), 150);
        QVERIFY(jamTrack->title.startsWith(QStringLiteral("Группа крови x")));
        QCOMPARE(jamTrack->artists.size(), 10);
        QCOMPARE(jamTrack->artists.at(3), QStringLiteral("A4"));
        QCOMPARE(jamTrack->durationMs, 0);
        QCOMPARE(jamTrack->coverUri, track.coverUri);
        QVERIFY(Jam::DecodeClient(Jam::Encode(Jam::Add{"a1", {}, jamTrack})).isValid());

        track.coverUri = QStringLiteral("https://avatars.yandex.net/a/400x400");
        QCOMPARE(Jam::JamTrackOf(track)->coverUri, QString());
        track.coverUri = QStringLiteral("avatars.yandex.net/a b/%%");
        QCOMPARE(Jam::JamTrackOf(track)->coverUri, QString());
        track.title = QStringLiteral("\n");
        QVERIFY(!Jam::JamTrackOf(track));
        track.title = QStringLiteral("T");
        track.id = QStringLiteral("123:456");
        QVERIFY(!Jam::JamTrackOf(track));
    }
};

QTEST_MAIN(TestJamHost)
#include "jam_host_test.moc"
