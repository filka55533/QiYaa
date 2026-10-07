#include "app/translations.h"
#include "audio/audio_engine.h"
#include "core/jam_mode.h"
#include "core/player.h"
#include "jam/host_session.h"
#include "jam/session_store.h"
#include "skins/skin.h"
#include "support/jam_stub_server.h"
#include "support/mock_http_server.h"
#include "support/spec_fixtures.h"
#include "ui/jam_window.h"
#include "yandex/api_client.h"
#include "yandex/library.h"

#include <QAction>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>

#include <memory>
#include <optional>
#include <vector>

// Ui::JamWindow against spec/jam/host.md (HOST-20, HOST-25, HOST-35): its controls, by their
// labels (the English source texts: no translation is installed), drive a HostSession on a stub
// server; its pictures on the built-in skins, in the default language, are compared with
// tests/data/golden, drawn with tests/data/fonts/Tiny5 so that every system draws the same.

namespace {

const QString kGoldenDir = QStringLiteral(QIYAA_TEST_DATA "/golden");

QJsonObject Example(const QString& name) {
    return Tests::SpecObject(QStringLiteral("jam/protocol/examples/%1.json").arg(name));
}

QByteArray Text(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject YandexTrack(const QString& id, const QString& title) {
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), title},
        {QStringLiteral("artists"),
         QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("КИНО")}}}},
        {QStringLiteral("albums"), QJsonArray{QJsonObject{{QStringLiteral("id"), 7}}}},
        {QStringLiteral("durationMs"), 200'000},
        {QStringLiteral("available"), true}
    };
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

QString DescribeDifference(const QImage& actual, const QImage& expected) {
    if (actual.size() != expected.size()) {
        return QStringLiteral("size %1x%2, expected %3x%4")
            .arg(actual.width())
            .arg(actual.height())
            .arg(expected.width())
            .arg(expected.height());
    }
    int differing = 0;
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            differing += actual.pixel(x, y) != expected.pixel(x, y);
        }
    }
    return differing == 0 ? QString() : QStringLiteral("%1 pixels differ").arg(differing);
}

}  // namespace

class TestJamWindow : public QObject {
    Q_OBJECT
private:
    Tests::MockHttpServer catalog;
    Tests::LocalNetworkAccessManager networkManager;
    Yandex::ApiClient api{&networkManager};
    Yandex::Library library{&api};
    Skins::Skin baseSkin = Skins::Skin::BuiltinBase();
    QFont pixelFont;

    struct Stack {
        Tests::JamStubServer server;
        QTemporaryDir directory;
        int ids = 0;
        bool greeted = false;
        Audio::AudioEngine engine;
        Core::Player player;
        Core::JamMode jam;
        Jam::HostSession host;
        Ui::JamWindow window;
        QSignalSpy status;

        Stack(Yandex::Library* library, const Skins::Skin* skin, const QFont& font)
            : player(library, &engine)
            , jam(&player, library)
            , host(
                  &jam,
                  library,
                  Jam::SessionStore(directory.filePath(QStringLiteral("jam.json"))),
                  options()
              )
            , window(&host, library, skin)
            , status(&window, &Ui::JamWindow::statusText) {
            window.setTextFont(font);
            window.setSizeSteps(QSize(1, 8));
            window.setServerName(QStringLiteral("jam.example.org"));
            window.show();
        }

        Jam::HostOptions options() {
            Jam::HostOptions options;
            options.client.appVersion = QStringLiteral("test");
            options.client.reconnectDelaysMs = {50, 100, 200};
            options.config = [this] { return Jam::HostConfig{server.serverUrl(), false}; };
            options.queueTitle = QStringLiteral("Джем");
            options.newId = [this] { return QStringLiteral("r%1").arg(++ids); };
            return options;
        }

        std::vector<Jam::ClientMessage> sent() const { return server.messages(); }

        // A jam on the server with the room of spec/jam/protocol/examples/state/host.
        bool created() {
            window.setHostName(QStringLiteral("Маша"));
            if (!click(QCoreApplication::translate("Ui::JamWindow", "Start the jam"))
                || !QTest::qWaitFor([&] { return !server.received().isEmpty(); }, 5000)) {
                return false;
            }
            server.welcome();
            if (!QTest::qWaitFor([&] { return server.received().size() >= 2; }, 5000)) {
                return false;
            }
            QJsonObject created = Example(QStringLiteral("created/ok"));
            created[QStringLiteral("id")] = QStringLiteral("r1");
            server.send(Text(created));
            server.send(Text(Example(QStringLiteral("state/host"))));
            return QTest::qWaitFor([&] { return host.room().has_value(); }, 5000);
        }

        std::optional<Ui::JamWindow::Control> control(const QString& label) {
            for (const Ui::JamWindow::Control& each : window.controls()) {
                if (each.label == label) {
                    return each;
                }
            }
            return std::nullopt;
        }

        // Clicks the control with this label, as a mouse would.
        bool click(const QString& label) {
            const std::optional<Ui::JamWindow::Control> target = control(label);
            if (!target) {
                qWarning("no control %s", qUtf8Printable(label));
                return false;
            }
            QTest::mouseClick(&window, Qt::LeftButton, {}, target->rect.center());
            return true;
        }
    };

    static QByteArray searchResults() {
        const QJsonArray tracks{
            YandexTrack(QStringLiteral("11"), QStringLiteral("Группа крови")),
            YandexTrack(QStringLiteral("22"), QStringLiteral("Кукушка"))
        };
        return Text({{QStringLiteral("tracks"), QJsonObject{{QStringLiteral("results"), tracks}}}});
    }

    void compareWithGolden(QWidget* window, const QString& name) {
        const QImage actual = window->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        const QString path = kGoldenDir + QLatin1Char('/') + name + QStringLiteral(".png");
        if (const QString shots = qEnvironmentVariable("QIYAA_TEST_SHOTS"); !shots.isEmpty()) {
            actual.save(shots + QLatin1Char('/') + name + QStringLiteral(".png"));
        }
        if (qEnvironmentVariable("QIYAA_UPDATE_GOLDEN") == QLatin1String("1")
            && !QFile::exists(path)) {
            QVERIFY(actual.save(path));
            QSKIP(qPrintable(QStringLiteral("recorded new golden image ") + path));
        }
        const QImage expected(path);
        QVERIFY2(!expected.isNull(), qPrintable(QStringLiteral("no golden image ") + path));
        const QString difference =
            DescribeDifference(actual, expected.convertToFormat(QImage::Format_ARGB32));
        QVERIFY2(difference.isEmpty(), qPrintable(name + QStringLiteral(": ") + difference));
    }

private Q_SLOTS:
    void initTestCase() {
        const int id =
            QFontDatabase::addApplicationFont(QStringLiteral(QIYAA_TEST_DATA
                                                             "/fonts/Tiny5-Regular.ttf"));
        QVERIFY(id >= 0);
        pixelFont = QFont(QFontDatabase::applicationFontFamilies(id).value(0));
        pixelFont.setPixelSize(8);
        pixelFont.setStyleStrategy(QFont::NoAntialias);
        pixelFont.setHintingPreference(QFont::PreferNoHinting);
        api.setBaseUrl(catalog.baseUrl());
        api.setToken(QStringLiteral("test-token"));
        catalog.fixture("GET", "/account/status", "account-status/ok");
        catalog.fixture("POST", "/rotor/session/new", "rotor-session-new/all-unavailable");
        bool done = false;
        library.connectAccount([&](const Yandex::Account&, const QString&) { done = true; });
        QVERIFY(QTest::qWaitFor([&] { return done; }, 5000));
    }

    void theStartPageStartsAJamWithTheGivenName() {
        Stack stack(&library, &baseSkin, pixelFont);
        QVERIFY(!stack.control(QStringLiteral("Start the jam"))->enabled);
        stack.window.setHostName(QStringLiteral("Маша"));
        QVERIFY(stack.control(QStringLiteral("Start the jam"))->enabled);
        QVERIFY(stack.click(QStringLiteral("Start the jam")));
        QCOMPARE(stack.host.phase(), Jam::HostPhase::Creating);
        QVERIFY(stack.control(QStringLiteral("Cancel")));
        QTRY_VERIFY(!stack.server.received().isEmpty());
        stack.server.welcome();
        QTRY_COMPARE(OfType<Jam::Create>(stack.sent()).size(), static_cast<size_t>(1));
        QCOMPARE(OfType<Jam::Create>(stack.sent()).front().hostName, QStringLiteral("Маша"));
    }

    void typingInAFieldDoesNotDriveThePlayer() {
        Stack stack(&library, &baseSkin, pixelFont);
        auto* play = new QAction(&stack.window);
        play->setShortcut(QKeySequence(Qt::Key_X));
        stack.window.addAction(play);
        QSignalSpy played(play, &QAction::triggered);
        QVERIFY(stack.click(QStringLiteral("How the guests will see you")));
        stack.window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&stack.window));
        QTest::keyClicks(&stack.window, QStringLiteral("Max"));
        QTest::keyClick(&stack.window, Qt::Key_Backspace);
        QCOMPARE(played.size(), 0);
        stack.window.setHostName(QStringLiteral("Маша"));  // typed by the user: not replaced
        QVERIFY(stack.click(QStringLiteral("Start the jam")));
        QTRY_VERIFY(!stack.server.received().isEmpty());
        stack.server.welcome();
        QTRY_COMPARE(OfType<Jam::Create>(stack.sent()).size(), static_cast<size_t>(1));
        QCOMPARE(OfType<Jam::Create>(stack.sent()).front().hostName, QStringLiteral("Ma"));
    }

    void theSettingsTheGuestsAndTheLinkGoToTheServer() {  // HOST-35
        Stack stack(&library, &baseSkin, pixelFont);
        QVERIFY(stack.created());
        QVERIFY(stack.click(QStringLiteral("Taking turns")));
        QVERIFY(stack.click(QStringLiteral("No")));
        QVERIFY(stack.click(QStringLiteral("Let in")));
        QVERIFY(stack.click(QStringLiteral("New link")));
        const std::optional<Ui::JamWindow::Control> kick = stack.control(QStringLiteral("Remove"));
        QVERIFY(kick);
        QTest::mouseClick(&stack.window, Qt::LeftButton, {}, kick->rect.center());
        QTRY_COMPARE(OfType<Jam::ChangeSettings>(stack.sent()).size(), static_cast<size_t>(3));
        const auto changes = OfType<Jam::ChangeSettings>(stack.sent());
        QCOMPARE(changes[0].settings.order, std::optional(Jam::Order::Fifo));
        QCOMPARE(changes[1].settings.guestsCanSkip, std::optional(true));
        QCOMPARE(changes[2].settings.joinOpen, std::optional(false));
        QTRY_COMPARE(OfType<Jam::RotateLink>(stack.sent()).size(), static_cast<size_t>(1));
        QTRY_COMPARE(OfType<Jam::Kick>(stack.sent()).size(), static_cast<size_t>(1));
        QCOMPARE(OfType<Jam::Kick>(stack.sent()).front().publicId, QStringLiteral("a4n8q1"));
    }

    void theLinkIsCopied() {  // HOST-35
        Stack stack(&library, &baseSkin, pixelFont);
        QVERIFY(stack.created());
        QVERIFY(stack.click(QStringLiteral("Copy")));
        QCOMPARE(QGuiApplication::clipboard()->text(), stack.host.joinUrl());
        QCOMPARE(stack.status.last().at(0).toString(), QStringLiteral("The link is copied"));
    }

    void theEndAsksTwice() {  // HOST-35
        Stack stack(&library, &baseSkin, pixelFont);
        QVERIFY(stack.created());
        QVERIFY(stack.click(QStringLiteral("End the jam")));
        QVERIFY(stack.control(QStringLiteral("Press again to end")));
        QCOMPARE(stack.host.phase(), Jam::HostPhase::Active);
        QVERIFY(stack.click(QStringLiteral("Press again to end")));
        QTRY_COMPARE(OfType<Jam::End>(stack.sent()).size(), static_cast<size_t>(1));
        QCOMPARE(stack.host.phase(), Jam::HostPhase::None);
        QVERIFY(stack.control(QStringLiteral("Start the jam")));
    }

    void withoutAConnectionOnlyTheLinkAndTheEndWork() {  // HOST-25
        Stack stack(&library, &baseSkin, pixelFont);
        QVERIFY(stack.created());
        stack.server.last().close();
        QTRY_VERIFY(!stack.host.isConnected());
        for (const QString& label :
             {QStringLiteral("New link"), QStringLiteral("Taking turns"), QStringLiteral("No"),
              QStringLiteral("Let in"), QStringLiteral("Remove")}) {
            QVERIFY2(!stack.control(label)->enabled, qUtf8Printable(label));
        }
        QVERIFY(stack.control(QStringLiteral("Copy"))->enabled);
        QVERIFY(stack.control(QStringLiteral("End the jam"))->enabled);
        stack.window.showPage(Ui::JamWindow::Page::Search);
        QVERIFY(stack.control(QStringLiteral("Jam"))->enabled);
    }

    void theSearchAddsAndPlaysNext() {  // HOST-20
        Stack stack(&library, &baseSkin, pixelFont);
        QVERIFY(stack.created());
        catalog.result("GET", "/search", searchResults());
        QVERIFY(stack.click(QStringLiteral("Add tracks")));
        QCOMPARE(stack.window.page(), Ui::JamWindow::Page::Search);
        for (const QChar letter : QStringLiteral("кино")) {
            QTest::sendKeyEvent(QTest::Click, &stack.window, Qt::Key_unknown, letter, {});
        }
        QTest::keyClick(&stack.window, Qt::Key_Return);
        QTRY_VERIFY(stack.control(QStringLiteral("To the jam")));
        QCOMPARE(
            catalog.last(QStringLiteral("/search"))->query.queryItemValue(QStringLiteral("text")),
            QStringLiteral("кино")
        );
        QVERIFY(stack.click(QStringLiteral("To the jam")));
        QTRY_COMPARE(OfType<Jam::Add>(stack.sent()).size(), static_cast<size_t>(1));
        QCOMPARE(OfType<Jam::Add>(stack.sent()).front().track->id, QStringLiteral("11"));
        QCOMPARE(stack.status.last().at(0).toString(), QStringLiteral("Sent to the jam"));
        QVERIFY(stack.click(QStringLiteral("Next")));
        QTRY_COMPARE(OfType<Jam::Add>(stack.sent()).size(), static_cast<size_t>(2));
    }

    void looksLikeTheSkin_data() {
        QTest::addColumn<QString>("skin");
        const QStringList skins =
            QDir(QStringLiteral(":/skins"))
                .entryList({QStringLiteral("*.wsz")}, QDir::Files, QDir::Name);
        QVERIFY(!skins.isEmpty());
        for (const QString& name : skins) {
            QTest::newRow(qPrintable(QStringLiteral("jam-") + QString(name).chopped(4)))
                << QStringLiteral(":/skins/") + name;
        }
    }

    void looksLikeTheSkin() {  // HOST-35
        QFETCH(QString, skin);
        App::Translations translations;  // the pictures are in the default language
        QVERIFY(translations.apply(App::kDefaultLanguage));
        const Skins::Skin loaded = Skins::Skin::LoadFile(skin, &baseSkin);
        Stack stack(&library, &loaded, pixelFont);
        QVERIFY(stack.created());
        compareWithGolden(&stack.window, QString::fromLatin1(QTest::currentDataTag()));
    }

    void theStartPageLooksAsRecorded() {
        App::Translations translations;  // the pictures are in the default language
        QVERIFY(translations.apply(App::kDefaultLanguage));
        Stack stack(&library, &baseSkin, pixelFont);
        stack.window.setHostName(QStringLiteral("Маша"));
        compareWithGolden(&stack.window, QStringLiteral("jam-start"));
    }

    void theSearchPageLooksAsRecorded() {  // HOST-20
        App::Translations translations;  // the pictures are in the default language
        QVERIFY(translations.apply(App::kDefaultLanguage));
        Stack stack(&library, &baseSkin, pixelFont);
        QVERIFY(stack.created());
        catalog.result("GET", "/search", searchResults());
        stack.window.showPage(Ui::JamWindow::Page::Search);
        stack.window.search(QStringLiteral("кино"));
        QTRY_VERIFY(stack.control(QCoreApplication::translate("Ui::JamWindow", "To the jam")));
        compareWithGolden(&stack.window, QStringLiteral("jam-search"));
    }

    void theOfflinePageLooksAsRecorded() {  // HOST-25
        App::Translations translations;  // the pictures are in the default language
        QVERIFY(translations.apply(App::kDefaultLanguage));
        Stack stack(&library, &baseSkin, pixelFont);
        QVERIFY(stack.created());
        // The connection drops; the next one waits for a welcome that does not come.
        stack.server.last().close();
        QTRY_COMPARE(stack.server.count(), 2);
        QTRY_VERIFY(!stack.server.received().isEmpty());
        QCOMPARE(stack.host.connection(), Jam::Status::Connecting);
        compareWithGolden(&stack.window, QStringLiteral("jam-offline"));
    }
};

QTEST_MAIN(TestJamWindow)
#include "jam_window_test.moc"
