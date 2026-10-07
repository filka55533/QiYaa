#include "app/application.h"
#include "app/translations.h"
#include "ui/playlist_window.h"

#include <QCoreApplication>
#include <QFile>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>

#include <memory>

// The interface languages: every text of translations/*.ts is translated with the same
// placeholders, the English texts say "vibe" for Yandex's "волна", the app starts in
// Belarusian unless told otherwise, and a new language applies while the app runs.

namespace {

struct Message {
    QString context;
    QString source;
    QStringList translations;  // one, or the plural forms
    QString type;  // "unfinished", "vanished", "obsolete" or empty
    bool numerus = false;
};

QList<Message> ReadTs(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("cannot read %s", qUtf8Printable(path));
    }
    QList<Message> messages;
    QXmlStreamReader xml(&file);
    QString context;
    Message message;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::EndElement && xml.name() == QLatin1String("message")) {
            messages << message;
        }
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }
        const QString name = xml.name().toString();
        if (name == QLatin1String("name")) {
            context = xml.readElementText();
        } else if (name == QLatin1String("message")) {
            message = Message{
                context,
                {},
                {},
                {},
                xml.attributes().value(QLatin1String("numerus")) == QLatin1String("yes")
            };
        } else if (name == QLatin1String("source")) {
            message.source = xml.readElementText();
        } else if (name == QLatin1String("translation")) {
            message.type = xml.attributes().value(QLatin1String("type")).toString();
            if (!message.numerus) {
                message.translations << xml.readElementText();
            }
        } else if (name == QLatin1String("numerusform")) {
            message.translations << xml.readElementText();
        }
    }
    if (xml.hasError()) {
        qFatal("%s: %s", qUtf8Printable(path), qUtf8Printable(xml.errorString()));
    }
    return messages;
}

QSet<QString> Placeholders(const QString& text) {
    static const QRegularExpression placeholder(QStringLiteral("%(\\d+|n)"));
    QSet<QString> found;
    for (auto match = placeholder.globalMatch(text); match.hasNext();) {
        found << match.next().captured();
    }
    return found;
}

QString TsPath(const QString& code) {
    return QStringLiteral(QIYAA_SOURCE_DIR "/translations/qiyaa_%1.ts").arg(code);
}

}  // namespace

class TestTranslations : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void everyTextIsTranslatedWithItsPlaceholders_data() {
        QTest::addColumn<QString>("code");
        QTest::addColumn<int>("forms");
        QTest::newRow("be") << QStringLiteral("be") << 3;
        QTest::newRow("ru") << QStringLiteral("ru") << 3;
    }

    void everyTextIsTranslatedWithItsPlaceholders() {
        QFETCH(QString, code);
        QFETCH(int, forms);
        const QList<Message> messages = ReadTs(TsPath(code));
        QVERIFY(messages.size() > 200);
        for (const Message& message : messages) {
            const QString where = message.context + QStringLiteral(": ") + message.source;
            QVERIFY2(
                message.type.isEmpty(), qUtf8Printable(message.type + QStringLiteral(" ") + where)
            );
            QCOMPARE(message.translations.size(), message.numerus ? forms : 1);
            for (const QString& translation : message.translations) {
                QVERIFY2(!translation.isEmpty(), qUtf8Printable(where));
                QVERIFY2(
                    Placeholders(translation) == Placeholders(message.source),
                    qUtf8Printable(where + QStringLiteral(" -> ") + translation)
                );
            }
        }
    }

    void theEnglishFileHoldsThePluralForms() {
        const QList<Message> plurals = ReadTs(TsPath(QStringLiteral("en")));
        QVERIFY(!plurals.isEmpty());
        for (const Message& message : plurals) {
            QVERIFY(message.numerus);
            QVERIFY(message.type.isEmpty());
            QCOMPARE(message.translations.size(), 2);
        }
    }

    // Yandex's "Моя волна" is "My Vibe" in English: no English text says "wave". The sources of a
    // translation file are all the English texts; the English file adds their plural forms.
    void englishSaysVibeNotWave() {
        QStringList english;
        for (const Message& message : ReadTs(TsPath(QStringLiteral("ru")))) {
            english << message.source;
        }
        for (const Message& message : ReadTs(TsPath(QStringLiteral("en")))) {
            english << message.translations;
        }
        QVERIFY(english.size() > 200);
        for (const QString& text : english) {
            QVERIFY2(
                !text.contains(QStringLiteral("wave"), Qt::CaseInsensitive), qUtf8Printable(text)
            );
        }
    }

    void theAppStartsInBelarusianOrInTheSavedLanguage() {
        QTemporaryDir directory;
        App::Application::Options options;
        options.offline = true;
        options.audio = false;
        options.mediaIntegration = false;
        options.settingsFile = directory.filePath(QStringLiteral("settings.ini"));
        {
            App::Application application(options);
            QCOMPARE(application.language(), App::Language::Belarusian);
            QCOMPARE(
                QCoreApplication::translate("App::Application", "Quit QiYaa"),
                QStringLiteral("Закрыць QiYaa")
            );
            application.setLanguage(App::Language::Russian);
        }
        App::Application application(options);
        QCOMPARE(application.language(), App::Language::Russian);
        QCOMPARE(
            QCoreApplication::translate("App::Application", "Quit QiYaa"),
            QStringLiteral("Закрыть QiYaa")
        );
    }

    void anotherLanguageAppliesWhileTheAppRuns() {
        App::Application::Options options;
        options.offline = true;
        options.audio = false;
        options.mediaIntegration = false;
        options.readOnlySettings = true;
        options.language = App::Language::English;
        App::Application application(options);
        Ui::PlaylistWindow* playlist = application.playlistWindow();
        QCOMPARE(playlist->windowTitle(), QStringLiteral("QiYaa: playlist"));
        application.setLanguage(App::Language::Belarusian);
        QTRY_COMPARE(playlist->windowTitle(), QStringLiteral("QiYaa: плэйліст"));
        QCOMPARE(
            QCoreApplication::translate("Ui::LibraryMenu", "My Vibe"), QStringLiteral("Мая хваля")
        );
        application.setLanguage(App::Language::English);
        QTRY_COMPARE(playlist->windowTitle(), QStringLiteral("QiYaa: playlist"));
        QCOMPARE(
            QCoreApplication::translate("Ui::LibraryMenu", "My Vibe"), QStringLiteral("My Vibe")
        );
    }

    void pluralsFollowTheLanguage_data() {
        QTest::addColumn<int>("language");
        QTest::addColumn<int>("count");
        QTest::addColumn<QString>("text");
        const auto be = static_cast<int>(App::Language::Belarusian);
        const auto ru = static_cast<int>(App::Language::Russian);
        const auto en = static_cast<int>(App::Language::English);
        QTest::newRow("be 1") << be << 1 << QStringLiteral("Мне падабаецца: 1 трэк");
        QTest::newRow("be 3") << be << 3 << QStringLiteral("Мне падабаецца: 3 трэкі");
        QTest::newRow("be 5") << be << 5 << QStringLiteral("Мне падабаецца: 5 трэкаў");
        QTest::newRow("be 21") << be << 21 << QStringLiteral("Мне падабаецца: 21 трэк");
        QTest::newRow("ru 2") << ru << 2 << QStringLiteral("Мне нравится: 2 трека");
        QTest::newRow("ru 11") << ru << 11 << QStringLiteral("Мне нравится: 11 треков");
        QTest::newRow("en 1") << en << 1 << QStringLiteral("Liked: 1 track");
        QTest::newRow("en 2") << en << 2 << QStringLiteral("Liked: 2 tracks");
    }

    void pluralsFollowTheLanguage() {
        QFETCH(int, language);
        QFETCH(int, count);
        QFETCH(QString, text);
        App::Translations translations;
        QVERIFY(translations.apply(static_cast<App::Language>(language)));
        QCOMPARE(
            QCoreApplication::translate("Core::Sources", "Liked: %n track(s)", nullptr, count), text
        );
    }
};

QTEST_MAIN(TestTranslations)
#include "translations_test.moc"
