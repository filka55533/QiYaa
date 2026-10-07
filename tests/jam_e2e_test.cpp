#include "app/application.h"
#include "jam/client.h"
#include "jam/codec.h"
#include "jam/host_session.h"
#include "support/mock_http_server.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QWebSocket>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

// The jam end to end: the app (offscreen, the Null audio output) hosts a jam on a real jam server,
// and a guest talks to that server as the web page does. QIYAA_JAM_TEST_SERVER is the server's
// address, the same as its PUBLIC_URL (CI runs the server's image); without it the test skips.

namespace {

const QString kTrack = QStringLiteral("11");

QByteArray Text(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject YandexTrack(const QString& id) {
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), QStringLiteral("Группа крови")},
        {QStringLiteral("artists"),
         QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("КИНО")}}}},
        {QStringLiteral("albums"), QJsonArray{QJsonObject{{QStringLiteral("id"), 7}}}},
        {QStringLiteral("durationMs"), 3'000},
        {QStringLiteral("available"), true}
    };
}

// A guest of the web page: its socket and what the server sent it.
class Guest : public QObject {
public:
    explicit Guest(const QString& serverUrl)
        : socket(QUrl(serverUrl).adjusted(QUrl::RemovePath).toString(QUrl::RemovePath)) {
        connect(&socket, &QWebSocket::textMessageReceived, this, [this](const QString& text) {
            const Jam::Decoded<Jam::ServerMessage> decoded = Jam::DecodeServer(text.toUtf8());
            if (decoded.message) {
                received.push_back(*decoded.message);
            } else {
                qWarning("guest: cannot decode %s", qUtf8Printable(text));
            }
        });
        socket.open(Jam::Client::SocketUrl(serverUrl));
    }

    bool connected() {
        return QTest::qWaitFor(
            [&] { return socket.state() == QAbstractSocket::ConnectedState; }, 10'000
        );
    }

    void send(const Jam::ClientMessage& message) {
        socket.sendTextMessage(QString::fromUtf8(Jam::Encode(message)));
    }

    template <typename T>
    std::optional<T> waitFor(std::function<bool(const T&)> matches = [](const T&) {
        return true;
    }) {
        std::optional<T> found;
        const bool arrived = QTest::qWaitFor(
            [&] {
                for (const Jam::ServerMessage& message : received) {
                    if (const T* typed = std::get_if<T>(&message); typed && matches(*typed)) {
                        found = *typed;
                        return true;
                    }
                }
                return false;
            },
            15'000
        );
        return arrived ? found : std::nullopt;
    }

    QWebSocket socket;
    std::vector<Jam::ServerMessage> received;
};

}  // namespace

class TestJamEndToEnd : public QObject {
    Q_OBJECT
private:
    QString serverUrl = qEnvironmentVariable("QIYAA_JAM_TEST_SERVER");
    Tests::MockHttpServer yandex;
    Tests::LocalNetworkAccessManager network;
    QTemporaryDir directory;
    std::unique_ptr<App::Application> application;

private Q_SLOTS:
    void initTestCase() {
        if (serverUrl.isEmpty()) {
            QSKIP("QIYAA_JAM_TEST_SERVER is not set");
        }
        QSettings settings(
            directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat
        );
        settings.setValue(QStringLiteral("jam/server"), serverUrl);
        settings.sync();

        yandex.fixture("GET", "/account/status", "account-status/ok");
        yandex.fixture("POST", "/rotor/session/new", "rotor-session-new/all-unavailable");
        yandex.result(
            "GET", "/search",
            Text(
                {{QStringLiteral("tracks"),
                  QJsonObject{{QStringLiteral("results"), QJsonArray{YandexTrack(kTrack)}}}}}
            )
        );
        yandex.result(
            "POST", "/tracks/",
            QJsonDocument(QJsonArray{YandexTrack(kTrack)}).toJson(QJsonDocument::Compact)
        );
        QFile file(QStringLiteral(QIYAA_TEST_DATA "/sine440_3s.mp3"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        yandex.audioTracks({kTrack}, file.readAll());

        App::Application::Options options;
        options.settingsFile = directory.filePath(QStringLiteral("settings.ini"));
        options.offline = true;  // no token lookup: the account is the MockHttpServer's
        options.mediaIntegration = false;
        options.network = &network;
        application = std::make_unique<App::Application>(options);
        application->start();
        application->api()->setBaseUrl(yandex.baseUrl());
        application->api()->setToken(QStringLiteral("test-token"));
        bool connected = false;
        application->library()->connectAccount([&](const Yandex::Account&, const QString& error) {
            connected = error.isEmpty();
        });
        QTRY_VERIFY_WITH_TIMEOUT(connected, 5000);
        connect(
            application->jamHost(), &Jam::HostSession::refused, this,
            [](const QString& reason) {
                qWarning("the jam server refused: %s", qUtf8Printable(reason));
            }
        );
    }

    void cleanupTestCase() {
        if (application && application->jamHost()) {
            application->jamHost()->end();  // a failed run leaves no live room behind (ROOM-02)
        }
        application.reset();
    }

    void aGuestSearchesAndAddsAndTheTrackPlaysWithoutPlayReports() {
        Jam::HostSession* host = application->jamHost();
        QVERIFY(host);
        QVERIFY(host->create(QStringLiteral("Хозяин")));
        QTRY_VERIFY_WITH_TIMEOUT(host->isConnected(), 15'000);
        QVERIFY(application->jam()->isActive());

        const QUrl joinUrl(host->joinUrl());
        Guest guest(serverUrl);
        QVERIFY(guest.connected());
        guest.send(Jam::Hello{Jam::kProtocol, Jam::App::Web, QStringLiteral("e2e")});
        QVERIFY(guest.waitFor<Jam::Welcome>());
        guest.send(Jam::Join{
            QStringLiteral("g1"), joinUrl.path().section(u'/', -1), joinUrl.fragment(),
            QStringLiteral("3f6c1a2e-8b4d-4c7a-9e21-5d0b7f3a1c64"), QStringLiteral("Аня")
        });
        QVERIFY(guest.waitFor<Jam::Joined>());

        guest.send(Jam::Search{QStringLiteral("g2"), QStringLiteral("кино")});
        const std::optional<Jam::SearchResults> found = guest.waitFor<Jam::SearchResults>();
        QVERIFY(found);
        QCOMPARE(found->tracks.size(), static_cast<size_t>(1));
        QCOMPARE(found->tracks.front().id, kTrack);
        QCOMPARE(
            yandex.last(QStringLiteral("/search"))->query.queryItemValue(QStringLiteral("type")),
            QStringLiteral("track")
        );

        guest.send(Jam::Add{QStringLiteral("g3"), kTrack, std::nullopt});
        QVERIFY(guest.waitFor<Jam::Ack>([](const Jam::Ack& ack) {
            return ack.id == QStringLiteral("g3");
        }));

        Core::Player* player = application->player();
        QTRY_VERIFY_WITH_TIMEOUT(
            player->currentTrack() && player->currentTrack()->id == kTrack, 15'000
        );
        QTRY_COMPARE_WITH_TIMEOUT(
            application->engine()->state(), Audio::AudioEngine::State::Playing, 15'000
        );
        const auto playing = guest.waitFor<Jam::State>([](const Jam::State& state) {
            const Jam::NowPlaying& now = state.room.nowPlaying;
            return now.source == Jam::Source::Item && now.track && now.track->id == kTrack;
        });
        QVERIFY(playing);
        QVERIFY(playing->room.queue.empty());  // started: the item left the queue
        QVERIFY2(!yandex.last(QStringLiteral("/play-audio")), "a jam track was reported to Yandex");

        host->end();
        QVERIFY(guest.waitFor<Jam::Ended>());
        QVERIFY(!application->jam()->isActive());

        // HOST-33: without the jam the same track is reported, so the check above means something.
        player->playIndex(player->currentIndex());
        QTRY_VERIFY_WITH_TIMEOUT(yandex.last(QStringLiteral("/play-audio")), 15'000);
    }
};

QTEST_MAIN(TestJamEndToEnd)
#include "jam_e2e_test.moc"
