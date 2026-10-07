#include "app/application.h"

#include "app/paths.h"
#include "audio/equalizer.h"
#include "core/cover_cache.h"
#include "integrations/media_controls.h"
#include "jam/host_session.h"
#include "skins/error.h"
#include "ui/equalizer_window.h"
#include "ui/gen_window.h"
#include "ui/jam_server_dialog.h"
#include "ui/jam_window.h"
#include "ui/library_menu.h"
#include "ui/login_dialog.h"
#include "ui/main_window.h"
#include "ui/milkdrop_window.h"
#include "ui/now_playing_window.h"
#include "ui/playlist_window.h"
#include "ui/snap.h"
#include "yandex/token.h"
#ifdef QIYAA_HAVE_MPRIS
#include "integrations/mpris.h"
#endif
#ifdef QIYAA_HAVE_SMTC
#include "integrations/smtc.h"
#endif
#ifdef QIYAA_HAVE_MAC_MEDIA_CONTROLS
#include "integrations/mac_media_controls.h"
#endif

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QKeySequence>
#include <QLatin1String>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkInformation>
#include <QPainter>
#include <QPoint>
#include <QPointF>
#include <QPushButton>
#include <QScreen>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <utility>

namespace App {

using Audio::EqSettings;

namespace {

QString SettingsPath(const Application::Options& options, QTemporaryDir* temporaryDirectory) {
    if (temporaryDirectory) {
        return temporaryDirectory->filePath(QStringLiteral("settings.ini"));
    }
    return options.settingsFile.isEmpty() ? App::ConfigDirectory() + QStringLiteral("/settings.ini")
                                          : options.settingsFile;
}

Audio::EqSettings ReadEq(const QSettings& settings) {
    Audio::EqSettings eq;
    eq.enabled = settings.value(QStringLiteral("equalizer/enabled"), true).toBool();
    eq.preampDb = settings.value(QStringLiteral("equalizer/preamp"), 0.0).toDouble();
    const QStringList bands = settings.value(QStringLiteral("equalizer/bands")).toStringList();
    for (int i = 0; i < Audio::kEqBands && i < bands.size(); ++i) {
        eq.bandsDb[i] = bands[i].toDouble();
    }
    return eq;
}

#ifdef QIYAA_HAVE_JAM
QString JamRefusalText(const QString& reason) {
    // Translated on each call: the language can change while the app runs.
    static const QHash<QString, const char*> texts = {
        {QStringLiteral("server-full"),
         QT_TRANSLATE_NOOP("App::Application", "The jam server is full, try later")},
        {QStringLiteral("rate-limited"),
         QT_TRANSLATE_NOOP("App::Application", "Too many requests, wait a little")},
        {QStringLiteral("update-required"),
         QT_TRANSLATE_NOOP("App::Application", "Update QiYaa to work with this jam server")},
        {QStringLiteral("not-allowed"),
         QT_TRANSLATE_NOOP("App::Application", "The jam server does not allow this")},
        {QStringLiteral("queue-limit"),
         QT_TRANSLATE_NOOP("App::Application", "The jam queue is full")},
        {QStringLiteral("duplicate"),
         QT_TRANSLATE_NOOP("App::Application", "This track is already in the jam queue")},
        {QStringLiteral("stale"),
         QT_TRANSLATE_NOOP("App::Application", "This track has already left the queue")},
    };
    return QCoreApplication::translate(
        "App::Application",
        texts.value(reason, QT_TRANSLATE_NOOP("App::Application", "The jam server refused"))
    );
}

QString JamServer(const QSettings& settings) {
    return settings.value(QStringLiteral("jam/server"), QStringLiteral(QIYAA_JAM_URL))
        .toString()
        .trimmed();
}

// What the jam window shows of the server: its host, and a port that is not the default one.
QString JamServerName(const QString& server) {
    const QUrl url(server);
    return url.port() > 0 ? QStringLiteral("%1:%2").arg(url.host()).arg(url.port()) : url.host();
}

QString JamEndText(Jam::HostEnd why) {
    switch (why) {
        case Jam::HostEnd::ByHost:
            return QCoreApplication::translate("App::Application", "The jam is over");
        case Jam::HostEnd::ByServer:
            return QCoreApplication::translate("App::Application", "The jam was ended");
        case Jam::HostEnd::Expired:
            return QCoreApplication::translate(
                "App::Application", "The jam closed while you were away"
            );
        case Jam::HostEnd::Gone:
            return QCoreApplication::translate(
                "App::Application", "The jam is over: the server no longer knows it"
            );
    }
    return {};
}
#endif

void WriteEq(QSettings& settings, const Audio::EqSettings& eq) {
    settings.setValue(QStringLiteral("equalizer/enabled"), eq.enabled);
    settings.setValue(QStringLiteral("equalizer/preamp"), eq.preampDb);
    QStringList bands;
    for (double bandDb : eq.bandsDb) {
        bands << QString::number(bandDb, 'f', 1);
    }
    settings.setValue(QStringLiteral("equalizer/bands"), bands);
}

}  // namespace

Application::Application(const Options& options, QObject* parent)
    : QObject(parent)
    , startOptions(options)
    , temporaryDirectory(options.readOnlySettings ? std::make_unique<QTemporaryDir>() : nullptr)
    , settings(SettingsPath(options, temporaryDirectory.get()), QSettings::IniFormat)
    , baseSkin(Skins::Skin::BuiltinBase())
    , currentSkin(std::make_unique<Skins::Skin>(baseSkin))
    , ownNetworkManager(options.network ? nullptr : std::make_unique<QNetworkAccessManager>())
    , networkManager(options.network ? options.network : ownNetworkManager.get())
    , apiClient(networkManager)
    , yandexLibrary(&apiClient)
    , corePlayer(&yandexLibrary, &audioEngine)
    , sources(&corePlayer, &yandexLibrary)
    , jamMode(&corePlayer, &yandexLibrary) {
    // Before any window: they take their texts from the translation.
    translations.apply(options.language.value_or(
        LanguageFromCode(settings.value(QStringLiteral("language")).toString())
            .value_or(kDefaultLanguage)
    ));
    corePlayer.setShuffleAlgorithm(
        settings.value(QStringLiteral("shuffle/algorithm")).toString() == QLatin1String("random")
            ? Core::ShuffleAlgorithm::Random
            : Core::ShuffleAlgorithm::WithoutRepeats
    );
    connect(&corePlayer, &Core::Player::modesChanged, this, [this] {
        settings.setValue(
            QStringLiteral("shuffle/algorithm"),
            corePlayer.shuffleAlgorithm() == Core::ShuffleAlgorithm::Random
                ? QStringLiteral("random")
                : QStringLiteral("without-repeats")
        );
    });
    if (startOptions.audio) {
        if (const Audio::AudioEngine::InitResult initResult = audioEngine.init(); !initResult.ok) {
            qWarning("Audio: %s", qPrintable(initResult.message));
        } else {
            qInfo("Audio backend: %s", qPrintable(audioEngine.backendName()));
        }
    }

    const QString skinPath = options.skinOverride.isEmpty()
        ? settings.value(QStringLiteral("skin")).toString()
        : options.skinOverride;
    if (!skinPath.isEmpty()) {
        try {
            currentSkin = std::make_unique<Skins::Skin>(Skins::Skin::LoadFile(skinPath, &baseSkin));
        } catch (const Skins::Error& error) {
            qWarning("Skin: %s; using the built-in one", error.what());
        }
    }

    mainWindowInstance = std::make_unique<Ui::MainWindow>(&corePlayer, currentSkin.get());
    equalizerWindowInstance = std::make_unique<Ui::EqualizerWindow>(currentSkin.get());
    playlistWindowInstance = std::make_unique<Ui::PlaylistWindow>(&corePlayer, currentSkin.get());
    coverCache = std::make_unique<Core::CoverCache>(
        networkManager,
        temporaryDirectory ? temporaryDirectory->filePath(QStringLiteral("covers")) : QString()
    );
    nowPlayingWindowInstance =
        std::make_unique<Ui::NowPlayingWindow>(&corePlayer, coverCache.get(), currentSkin.get());
    for (QWidget* widget : std::initializer_list<QWidget*>{
             mainWindowInstance.get(), equalizerWindowInstance.get(), playlistWindowInstance.get(),
             nowPlayingWindowInstance.get()
         }) {
        installShortcuts(widget);
    }
    equalizerWindowInstance->setSecondary();
    playlistWindowInstance->setSecondary();
    nowPlayingWindowInstance->setSecondary();
    nowPlayingWindowInstance->setSizeSteps(
        settings.value(QStringLiteral("nowPlaying/steps"), QSize(0, 0)).toSize()
    );
    connect(nowPlayingWindowInstance.get(), &Ui::NowPlayingWindow::closeRequested, this, [this] {
        setNowPlayingVisible(false);
    });
    connect(
        nowPlayingWindowInstance.get(), &Ui::GenWindow::sizeStepsChanged, this,
        [this](QSize steps) { settings.setValue(QStringLiteral("nowPlaying/steps"), steps); }
    );
#if defined(QIYAA_HAVE_MILKDROP)
    {
        const QString userPresets =
            (temporaryDirectory ? temporaryDirectory->path() : App::ConfigDirectory())
            + QStringLiteral("/milkdrop");
        milkdropWindowInstance = std::make_unique<Ui::MilkdropWindow>(
            &audioEngine, QStringLiteral(":/milkdrop"), userPresets, currentSkin.get()
        );
        installShortcuts(milkdropWindowInstance.get());
        milkdropWindowInstance->setSecondary();
        milkdropWindowInstance->setSizeSteps(
            settings.value(QStringLiteral("milkdrop/steps"), QSize(0, 4)).toSize()
        );
        milkdropWindowInstance->setShuffle(
            settings.value(QStringLiteral("milkdrop/shuffle"), true).toBool()
        );
        milkdropWindowInstance->setLocked(
            settings.value(QStringLiteral("milkdrop/locked"), false).toBool()
        );
        milkdropWindowInstance->setPresetSeconds(
            settings.value(QStringLiteral("milkdrop/seconds"), 30).toInt()
        );
        milkdropWindowInstance->setBlackPresets(
            settings.value(QStringLiteral("milkdrop/black")).toStringList()
        );
        milkdropWindowInstance->selectPreset(
            settings.value(QStringLiteral("milkdrop/preset")).toString()
        );
        connect(milkdropWindowInstance.get(), &Ui::MilkdropWindow::closeRequested, this, [this] {
            setMilkdropVisible(false);
        });
        connect(
            milkdropWindowInstance.get(), &Ui::GenWindow::sizeStepsChanged, this,
            [this](QSize steps) { settings.setValue(QStringLiteral("milkdrop/steps"), steps); }
        );
        connect(milkdropWindowInstance.get(), &Ui::MilkdropWindow::settingsChanged, this, [this] {
            settings.setValue(
                QStringLiteral("milkdrop/shuffle"), milkdropWindowInstance->shuffle()
            );
            settings.setValue(QStringLiteral("milkdrop/locked"), milkdropWindowInstance->locked());
            settings.setValue(
                QStringLiteral("milkdrop/seconds"), milkdropWindowInstance->presetSeconds()
            );
            settings.setValue(
                QStringLiteral("milkdrop/preset"), milkdropWindowInstance->currentPreset()
            );
            settings.setValue(
                QStringLiteral("milkdrop/black"), milkdropWindowInstance->blackPresets()
            );
        });
        connect(
            milkdropWindowInstance.get(), &Ui::MilkdropWindow::presetChanged, this,
            [this](const QString& name, Ui::MilkdropWindow::PresetOrigin origin) {
                if (origin == Ui::MilkdropWindow::PresetOrigin::User) {
                    mainWindowInstance->setStatusText(QStringLiteral("Milkdrop: ") + name);
                }
            }
        );
        connect(
            milkdropWindowInstance.get(), &Ui::MilkdropWindow::transportKey, this,
            &Application::transportKey
        );
        connect(
            &audioEngine, &Audio::AudioEngine::stateChanged, milkdropWindowInstance.get(),
            [this](Audio::AudioEngine::State state) {
                milkdropWindowInstance->setPlaying(state == Audio::AudioEngine::State::Playing);
            }
        );
    }
#endif

    // Before the loops over windows() below, which take in the jam window.
    sources.setJamMode(&jamMode);
    setUpJam();

    mainWindowInstance->setVolume(settings.value(QStringLiteral("volume"), 75).toInt());
    mainWindowInstance->setBalance(settings.value(QStringLiteral("balance"), 0).toInt());
    mainWindowInstance->setVisMode(static_cast<Ui::MainWindow::VisMode>(
        std::clamp(settings.value(QStringLiteral("vis/mode"), 0).toInt(), 0, 2)
    ));
    mainWindowInstance->setShowsRemainingTime(
        settings.value(QStringLiteral("time/remaining"), false).toBool()
    );
    connect(mainWindowInstance.get(), &Ui::MainWindow::eqToggleRequested, this, [this] {
        setEqualizerVisible(!equalizerWindowInstance->isVisible());
    });
    connect(mainWindowInstance.get(), &Ui::MainWindow::playlistToggleRequested, this, [this] {
        setPlaylistVisible(!playlistWindowInstance->isVisible());
    });
    connect(mainWindowInstance.get(), &Ui::MainWindow::menuRequested, this, [this](QPoint pos) {
        showMainMenu(pos, corePlayer.currentTrack());
    });
    connect(
        mainWindowInstance.get(), &Ui::MainWindow::sourcesMenuRequested, this,
        &Application::showSourcesMenu
    );
    connect(mainWindowInstance.get(), &Ui::MainWindow::closeRequested, this, &Application::quit);
    connect(
        mainWindowInstance.get(), &Ui::MainWindow::minimizedChanged, this,
        [this](bool minimized) {
            if (minimized) {
                equalizerWindowInstance->hide();
                playlistWindowInstance->hide();
                nowPlayingWindowInstance->hide();
                if (milkdropWindowInstance) {
                    milkdropWindowInstance->hide();
                }
                if (jamWindowInstance) {
                    jamWindowInstance->hide();
                }
            } else {
                if (settings.value(QStringLiteral("equalizer/visible"), true).toBool()) {
                    equalizerWindowInstance->show();
                }
                if (settings.value(QStringLiteral("playlist/visible"), true).toBool()) {
                    playlistWindowInstance->show();
                }
                if (settings.value(QStringLiteral("nowPlaying/visible"), false).toBool()) {
                    nowPlayingWindowInstance->show();
                }
                if (milkdropWindowInstance
                    && settings.value(QStringLiteral("milkdrop/visible"), false).toBool()) {
                    milkdropWindowInstance->show();
                }
                if (jamWindowInstance
                    && settings.value(QStringLiteral("jamWindow/visible"), false).toBool()) {
                    jamWindowInstance->show();
                }
            }
        }
    );
    connect(mainWindowInstance.get(), &Ui::MainWindow::activated, this, &Application::raiseWindows);

    const Audio::EqSettings eq = ReadEq(settings);
    equalizerWindowInstance->setSettings(eq);
    equalizerWindowInstance->setAutoOn(
        settings.value(QStringLiteral("equalizer/auto"), false).toBool()
    );
    audioEngine.setEqualizer(eq);
    connect(
        equalizerWindowInstance.get(), &Ui::EqualizerWindow::settingsChanged, this,
        [this](const Audio::EqSettings& equalizerSettings) {
            audioEngine.setEqualizer(equalizerSettings);
            WriteEq(settings, equalizerSettings);
        }
    );
    connect(
        equalizerWindowInstance.get(), &Ui::EqualizerWindow::statusText, mainWindowInstance.get(),
        &Ui::MainWindow::setStatusText
    );
    equalizerWindowInstance->setMixer(mainWindowInstance->volume(), mainWindowInstance->balance());
    connect(mainWindowInstance.get(), &Ui::MainWindow::volumeChanged, this, [this](int volume) {
        equalizerWindowInstance->setMixer(volume, mainWindowInstance->balance());
    });
    connect(mainWindowInstance.get(), &Ui::MainWindow::balanceChanged, this, [this](int balance) {
        equalizerWindowInstance->setMixer(mainWindowInstance->volume(), balance);
    });
    connect(
        equalizerWindowInstance.get(), &Ui::EqualizerWindow::volumeRequested,
        mainWindowInstance.get(), &Ui::MainWindow::setVolume
    );
    connect(
        equalizerWindowInstance.get(), &Ui::EqualizerWindow::balanceRequested,
        mainWindowInstance.get(), &Ui::MainWindow::setBalance
    );
    connect(equalizerWindowInstance.get(), &Ui::EqualizerWindow::closeRequested, this, [this] {
        setEqualizerVisible(false);
    });

    playlistWindowInstance->setSizeSteps(
        settings.value(QStringLiteral("playlist/steps"), QSize(0, 4)).toSize()
    );
    connect(playlistWindowInstance.get(), &Ui::PlaylistWindow::closeRequested, this, [this] {
        setPlaylistVisible(false);
    });
    connect(
        playlistWindowInstance.get(), &Ui::PlaylistWindow::sourcesMenuRequested, this,
        [this](QPoint pos) { showMainMenu(pos, corePlayer.currentTrack()); }
    );
    connect(
        playlistWindowInstance.get(), &Ui::PlaylistWindow::trackMenuRequested, this,
        [this](QPoint pos, int row) {
            const auto& tracks = corePlayer.playlist();
            showMainMenu(pos, row >= 0 && row < tracks.size() ? &tracks[row] : nullptr);
        }
    );
    connect(
        playlistWindowInstance.get(), &Ui::PlaylistWindow::sizeStepsChanged, this,
        [this](QSize steps) { settings.setValue(QStringLiteral("playlist/steps"), steps); }
    );

    for (Ui::SkinnedWindow* window : windows()) {
        connect(window, &Ui::SkinnedWindow::moveFinished, this, &Application::saveState);
    }

    const std::pair<Ui::SkinnedWindow*, QString> shades[] = {
        {mainWindowInstance.get(), QStringLiteral("mainWindow/shaded")},
        {equalizerWindowInstance.get(), QStringLiteral("equalizer/shaded")},
        {playlistWindowInstance.get(), QStringLiteral("playlist/shaded")}
    };
    for (const auto& [window, key] : shades) {
        window->setShaded(settings.value(key, false).toBool());
        connect(window, &Ui::SkinnedWindow::shadeChanged, this, [this, key](bool on) {
            settings.setValue(key, on);
            saveState();
        });
    }

    const double scale = settings.value(QStringLiteral("scale"), 1.0).toDouble();
    for (Ui::SkinnedWindow* window : windows()) {
        window->setScale(scale);
    }
    if (settings.value(QStringLiteral("alwaysOnTop"), false).toBool()) {
        setAlwaysOnTop(true);
    }

    if (startOptions.mediaIntegration) {
        Integrations::MediaControls::Hooks hooks;
        hooks.volume = [this] { return mainWindowInstance->volume(); };
        hooks.setVolume = [this](int volume) { mainWindowInstance->setVolume(volume); };
        hooks.raise = [this] {
            if (mainWindowInstance->isMinimized()) {
                mainWindowInstance->showNormal();
            }
            raiseWindows();
            mainWindowInstance->activateWindow();
        };
        hooks.quit = [this] { quit(); };
        mediaControls =
            std::make_unique<Integrations::MediaControls>(&corePlayer, coverCache.get(), hooks);
        connect(
            mainWindowInstance.get(), &Ui::MainWindow::volumeChanged, mediaControls.get(),
            &Integrations::MediaControls::volumeChanged
        );
#if defined(QIYAA_HAVE_MPRIS)
        systemMediaControls = std::make_unique<Integrations::Mpris>(mediaControls.get());
#elif defined(QIYAA_HAVE_SMTC)
        systemMediaControls =
            std::make_unique<Integrations::Smtc>(mediaControls.get(), mainWindowInstance.get());
#elif defined(QIYAA_HAVE_MAC_MEDIA_CONTROLS)
        systemMediaControls = std::make_unique<Integrations::MacMediaControls>(mediaControls.get());
#endif
    }

    // The system's view of the network ends a wait for it at once (spec ERR-02); without a
    // backend that can tell, the Player retries on its timer alone.
    if (startOptions.audio && !startOptions.offline) {
        watchNetwork();
    }

    connect(qApp, &QApplication::aboutToQuit, this, [this] {
        saveState();
        corePlayer.stop();
    });
}

Application::~Application() = default;

void Application::watchNetwork() {
    using Information = QNetworkInformation;
    if (!Information::loadDefaultBackend()
        && !Information::loadBackendByFeatures(Information::Feature::Reachability)) {
        return;
    }
    Information* information = Information::instance();
    if (!information || !information->supports(Information::Feature::Reachability)) {
        return;
    }
    auto apply = [this](Information::Reachability reachability) {
        // Local and Site say little about the Yandex servers: leave those to the timer.
        corePlayer.setNetworkOnline(
            reachability == Information::Reachability::Online             ? std::optional(true)
                : reachability == Information::Reachability::Disconnected ? std::optional(false)
                                                                          : std::nullopt
        );
#ifdef QIYAA_HAVE_JAM
        if (jamHostSession && reachability == Information::Reachability::Online) {
            jamHostSession->networkBack();  // HOST-26
        }
#endif
    };
    connect(information, &Information::reachabilityChanged, this, apply);
    apply(information->reachability());
}

void Application::setUpJam() {
#ifdef QIYAA_HAVE_JAM
    Jam::HostOptions options;
    options.client.appVersion = QCoreApplication::applicationVersion();
    options.config = [this] {
        return Jam::HostConfig{
            JamServer(settings), settings.value(QStringLiteral("jam/waveFeedback"), true).toBool(),
            settings.value(QStringLiteral("jam/shareAudio"), false).toBool()
        };
    };
    options.queueTitle = tr("Jam");
    // Next to the settings: a test's or a read-only run's jam stays in its own directory.
    const Jam::SessionStore store(
        QFileInfo(settings.fileName()).dir().filePath(QStringLiteral("jam-session.json"))
    );
    jamHostSession =
        std::make_unique<Jam::HostSession>(&jamMode, &yandexLibrary, store, std::move(options));

    jamWindowInstance =
        std::make_unique<Ui::JamWindow>(jamHostSession.get(), &yandexLibrary, currentSkin.get());
    installShortcuts(jamWindowInstance.get());
    jamWindowInstance->setSecondary();
    jamWindowInstance->setSizeSteps(
        settings.value(QStringLiteral("jamWindow/steps"), QSize(1, 8)).toSize()
    );
    jamWindowInstance->setServerName(JamServerName(JamServer(settings)));
    connect(jamWindowInstance.get(), &Ui::JamWindow::closeRequested, this, [this] {
        setJamWindowVisible(false);
    });
    connect(jamWindowInstance.get(), &Ui::GenWindow::sizeStepsChanged, this, [this](QSize steps) {
        settings.setValue(QStringLiteral("jamWindow/steps"), steps);
    });
    connect(
        jamWindowInstance.get(), &Ui::JamWindow::statusText, mainWindowInstance.get(),
        &Ui::MainWindow::setStatusText
    );
    connect(
        jamWindowInstance.get(), &Ui::JamWindow::serverSettingsRequested, this,
        &Application::showJamServerDialog
    );

    // HOST-34: who added each jam item, and the jam wave, in the playlist and the marquee.
    const auto note = [this](int row) -> QString {
        const QList<Core::JamSlot>& slots = jamMode.queueSlots();
        if (!jamMode.isActive() || row < 0 || row >= slots.size()) {
            return {};
        }
        const Core::JamSlot& slot = slots[row];
        if (slot.kind == Core::JamSlot::Kind::Wave) {
            return tr("jam vibe");
        }
        if (slot.kind != Core::JamSlot::Kind::Item || !jamHostSession->room()) {
            return {};
        }
        for (const Jam::Participant& participant : jamHostSession->room()->participants) {
            if (participant.publicId == slot.addedBy) {
                return QStringLiteral("+ ") + participant.name;
            }
        }
        return {};
    };
    mainWindowInstance->setTrackNote(note);
    Ui::PlaylistWindow::QueueHooks hooks;
    hooks.note = note;
    // HOST-21: during a jam the selected jam items leave the room's queue, and nothing else
    // changes the queue.
    hooks.remove = [this](const QList<int>& rows) {
        if (!jamMode.isActive()) {
            return false;
        }
        QStringList items;
        for (int row : rows) {
            if (row >= 0 && row < jamMode.queueSlots().size()
                && jamMode.queueSlots()[row].kind == Core::JamSlot::Kind::Item) {
                items << jamMode.queueSlots()[row].itemId;
            }
        }
        if (items.isEmpty()) {
            mainWindowInstance->setStatusText(tr("Select jam tracks to remove them"));
        } else if (!std::all_of(items.cbegin(), items.cend(), [this](const QString& itemId) {
                       return jamHostSession->remove(itemId);
                   })) {
            mainWindowInstance->setStatusText(tr("No connection to the jam server"));
        }
        return true;
    };
    hooks.clear = [this] {
        if (!jamMode.isActive()) {
            return false;
        }
        mainWindowInstance->setStatusText(tr("A jam is on: add tracks to the jam"));
        return true;
    };
    playlistWindowInstance->setQueueHooks(std::move(hooks));
    connect(jamHostSession.get(), &Jam::HostSession::refused, this, [this](const QString& reason) {
        mainWindowInstance->setStatusText(JamRefusalText(reason));
    });
    connect(jamHostSession.get(), &Jam::HostSession::ended, this, [this](Jam::HostEnd why) {
        mainWindowInstance->setStatusText(JamEndText(why));
    });
    // HOST-25, HOST-26: the status follows the connection while the jam is on.
    connect(
        jamHostSession.get(), &Jam::HostSession::changed, this,
        [this, wasConnected = false]() mutable {
            const bool active = jamHostSession->phase() == Jam::HostPhase::Active;
            const bool connected = active && jamHostSession->isConnected();
            playlistWindowInstance->update();
            if (active && wasConnected && !connected) {
                mainWindowInstance->setStatusText(
                    tr("No connection to the jam server · trying again by itself")
                );
            } else if (connected && !wasConnected) {
                mainWindowInstance->setStatusText(tr("The jam is connected"));
            }
            wasConnected = connected;
        }
    );
#endif
}

void Application::offerStoredJam() {
#ifdef QIYAA_HAVE_JAM
    if (!jamHostSession || !jamHostSession->hasStoredSession()) {
        return;
    }
    // HOST-23. Not modal: playback and the windows work while the question waits.
    auto* question = new QMessageBox(
        QMessageBox::Question, tr("A jam was on"),
        tr("Continue it? The guests' link stays the same."), QMessageBox::NoButton,
        mainWindowInstance.get()
    );
    question->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton* yes = question->addButton(tr("Continue"), QMessageBox::AcceptRole);
    QPushButton* no = question->addButton(tr("End it"), QMessageBox::DestructiveRole);
    question->setDefaultButton(yes);
    connect(question, &QMessageBox::finished, this, [this, question, yes, no] {
        // Closed without an answer: the jam stays stored, and the next start asks again.
        if (question->clickedButton() == yes) {
            jamHostSession->continueStored();
            setJamWindowVisible(true);
        } else if (question->clickedButton() == no) {
            jamHostSession->discardStored();
            if (corePlayer.playlist().isEmpty() && yandexLibrary.isLoggedIn()) {
                sources.playLikes(false);
            }
        }
    });
    storedJamQuestion = question;
    question->open();
#endif
}

QList<Ui::SkinnedWindow*> Application::windows() const {
    QList<Ui::SkinnedWindow*> out{
        mainWindowInstance.get(), equalizerWindowInstance.get(), playlistWindowInstance.get(),
        nowPlayingWindowInstance.get()
    };
    if (milkdropWindowInstance) {
        out << milkdropWindowInstance.get();
    }
    if (jamWindowInstance) {
        out << jamWindowInstance.get();
    }
    return out;
}

void Application::raiseWindows() {
    for (Ui::SkinnedWindow* window : windows()) {
        if (window != mainWindowInstance.get() && window->isVisible()) {
            window->raise();
        }
    }
    mainWindowInstance->raise();
}

void Application::layoutWindows() {
    const QPoint mainPosition =
        settings.value(QStringLiteral("mainWindow/pos"), QPoint(100, 100)).toPoint();
    const QPoint equalizerDefault = mainPosition + QPoint(0, mainWindowInstance->height());
    const QPoint playlistDefault = equalizerDefault + QPoint(0, equalizerWindowInstance->height());
    mainWindowInstance->placeAt(mainPosition);
    equalizerWindowInstance->placeAt(
        settings.value(QStringLiteral("equalizer/pos"), equalizerDefault).toPoint()
    );
    playlistWindowInstance->placeAt(
        settings.value(QStringLiteral("playlist/pos"), playlistDefault).toPoint()
    );
    nowPlayingWindowInstance->placeAt(settings
                                          .value(
                                              QStringLiteral("nowPlaying/pos"),
                                              mainPosition + QPoint(mainWindowInstance->width(), 0)
                                          )
                                          .toPoint());
    if (milkdropWindowInstance) {
        milkdropWindowInstance->placeAt(
            settings
                .value(
                    QStringLiteral("milkdrop/pos"),
                    mainPosition + QPoint(mainWindowInstance->width(), mainWindowInstance->height())
                )
                .toPoint()
        );
    }
    if (jamWindowInstance) {
        jamWindowInstance->placeAt(settings
                                       .value(
                                           QStringLiteral("jamWindow/pos"),
                                           mainPosition + QPoint(mainWindowInstance->width(), 0)
                                       )
                                       .toPoint());
    }
}

void Application::start() {
    mainWindowInstance->show();
    if (settings.value(QStringLiteral("equalizer/visible"), true).toBool()) {
        equalizerWindowInstance->show();
    }
    if (settings.value(QStringLiteral("playlist/visible"), true).toBool()) {
        playlistWindowInstance->show();
    }
    if (settings.value(QStringLiteral("nowPlaying/visible"), false).toBool()) {
        nowPlayingWindowInstance->show();
    }
    if (milkdropWindowInstance
        && settings.value(QStringLiteral("milkdrop/visible"), false).toBool()) {
        milkdropWindowInstance->show();
    }
    layoutWindows();
    mainWindowInstance->setEqButton(equalizerWindowInstance->isVisible());
    mainWindowInstance->setPlaylistButton(playlistWindowInstance->isVisible());
    mainWindowInstance->activateWindow();

    if (startOptions.offline) {
        return;
    }
    offerStoredJam();
    const Yandex::TokenSource token = Yandex::FindToken(TokenFile(), YaampTokenFiles());
    if (token.token.isEmpty()) {
        mainWindowInstance->setStatusText(tr("Log in: right click → Log in"));
        QTimer::singleShot(0, this, &Application::login);
        return;
    }
    qInfo("Using Yandex token from %s", qPrintable(token.origin));
    const bool imported = !token.origin.startsWith(App::ConfigDirectory())
        && !token.origin.startsWith(QLatin1String("environment"));
    applyToken(token.token, imported);
}

void Application::applyToken(const QString& token, bool save) {
    apiClient.setToken(token);
    mainWindowInstance->setStatusText(tr("Connecting to Yandex Music…"));
    yandexLibrary.connectAccount([this, token,
                                  save](const Yandex::Account& account, const QString& error) {
        if (!error.isEmpty()) {
            mainWindowInstance->setStatusText(tr("Login failed: %1").arg(error));
            return;
        }
        if (save) {
            Yandex::SaveToken(TokenFile(), token);
        }
        mainWindowInstance->setStatusText(tr("Hello, %1!").arg(account.displayName));
#ifdef QIYAA_HAVE_JAM
        jamWindowInstance->setHostName(account.displayName);
#endif
        const bool jam = jamMode.isActive() || storedJamQuestion;
        if (corePlayer.playlist().isEmpty() && !jam) {
            sources.playLikes(false);
        }
    });
}

void Application::login() {
    Ui::LoginDialog dialog(networkManager, mainWindowInstance.get());
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    applyToken(dialog.token(), true);
}

void Application::logout() {
#ifdef QIYAA_HAVE_JAM
    if (jamHostSession) {
        // HOST-32: the guests search through this account.
        jamHostSession->cancelCreate();
        jamHostSession->end();
    }
#endif
    corePlayer.clearQueue();
    yandexLibrary.logout();
    Yandex::ForgetToken(TokenFile());
    mainWindowInstance->setStatusText(tr("You have logged out"));
}

bool Application::loadSkin(const QString& path) {
    std::unique_ptr<Skins::Skin> skin;
    try {
        skin = std::make_unique<Skins::Skin>(Skins::Skin::LoadFile(path, &baseSkin));
    } catch (const Skins::Error& error) {
        mainWindowInstance->setStatusText(
            tr("Cannot load the skin: %1").arg(QString::fromUtf8(error.what()))
        );
        return false;
    }
    // Swap after the windows point at the new skin.
    for (Ui::SkinnedWindow* window : windows()) {
        window->setSkin(skin.get());
    }
    currentSkin = std::move(skin);
    settings.setValue(QStringLiteral("skin"), path);
    return true;
}

void Application::setScale(double scale, ScaleScope scope) {
    const double oldScale = mainWindowInstance->scale();
    const QList<Ui::SkinnedWindow*> all = windows();
    QList<QRect> frames;
    for (Ui::SkinnedWindow* window : all) {
        frames << window->frameGeometry();
    }
    QList<std::pair<Ui::SkinnedWindow*, QPoint>> docked;
    for (int i : Ui::ConnectedGroup(0, frames)) {
        const QPointF offset = QPointF(all[i]->pos() - mainWindowInstance->pos()) / oldScale;
        docked.append({all[i], QPoint(qRound(offset.x()), qRound(offset.y()))});
    }

    for (Ui::SkinnedWindow* window : all) {
        window->setScale(scale);
    }
    const double newScale = mainWindowInstance->scale();
    const QPoint mainPosition = mainWindowInstance->pos();

    QList<QRect> placed{mainWindowInstance->frameGeometry()};
    for (const auto& [window, offset] : docked) {
        QRect frame(
            mainPosition + QPoint(qRound(offset.x() * newScale), qRound(offset.y() * newScale)),
            window->size()
        );
        frame.moveTopLeft(Ui::SnapToOthers(frame, placed, 4));
        window->move(frame.topLeft());
        placed << frame;
    }
    QRect bounds = mainWindowInstance->frameGeometry();
    for (const auto& [window, offset] : docked) {
        if (window->isVisible()) {
            bounds |= window->frameGeometry();
        }
    }
    QList<QRect> screens;
    for (QScreen* monitor : QGuiApplication::screens()) {
        screens << monitor->availableGeometry();
    }
    const QRect screen = Ui::PickScreen(bounds, screens);
    const QPoint shift = Ui::ClampInside(bounds, screen) - bounds.topLeft();
    if (!shift.isNull()) {
        mainWindowInstance->move(mainWindowInstance->pos() + shift);
        for (const auto& [window, offset] : docked) {
            window->move(window->pos() + shift);
        }
    }
    if (!screen.isEmpty()
        && (bounds.width() > screen.width() || bounds.height() > screen.height())) {
        for (Ui::SkinnedWindow* window : all) {
            window->ensureVisible();
        }
    }

    transientScale = scope == ScaleScope::ThisRun;
    if (scope == ScaleScope::Saved) {
        settings.setValue(QStringLiteral("scale"), newScale);
        saveState();
    }
}

void Application::setLanguage(Language language) {
    // Qt sends LanguageChange to every widget; the windows draw their texts again.
    translations.apply(language);
    settings.setValue(QStringLiteral("language"), LanguageCode(language));
}

void Application::setAlwaysOnTop(bool on) {
    for (Ui::SkinnedWindow* window : windows()) {
        const bool visible = window->isVisible();
        window->setWindowFlag(Qt::WindowStaysOnTopHint, on);
        if (visible) {
            window->show();  // changing flags hides the window
        }
    }
    settings.setValue(QStringLiteral("alwaysOnTop"), on);
}

void Application::setEqualizerVisible(bool on) {
    equalizerWindowInstance->setVisible(on);
    if (on) {
        equalizerWindowInstance->ensureVisible();
    }
    mainWindowInstance->setEqButton(on);
    settings.setValue(QStringLiteral("equalizer/visible"), on);
}

void Application::setPlaylistVisible(bool on) {
    playlistWindowInstance->setVisible(on);
    if (on) {
        playlistWindowInstance->ensureVisible();
    }
    mainWindowInstance->setPlaylistButton(on);
    settings.setValue(QStringLiteral("playlist/visible"), on);
}

void Application::setMilkdropVisible(bool on) {
    if (!milkdropWindowInstance) {
        return;
    }
    milkdropWindowInstance->setVisible(on);
    if (on) {
        milkdropWindowInstance->ensureVisible();
    }
    settings.setValue(QStringLiteral("milkdrop/visible"), on);
}

void Application::setJamWindowVisible(bool on) {
    if (!jamWindowInstance) {
        return;
    }
    jamWindowInstance->setVisible(on);
    if (on) {
        jamWindowInstance->ensureVisible();
        jamWindowInstance->raise();
    }
    settings.setValue(QStringLiteral("jamWindow/visible"), on);
}

void Application::setNowPlayingVisible(bool on) {
    nowPlayingWindowInstance->setVisible(on);
    if (on) {
        nowPlayingWindowInstance->ensureVisible();
    }
    settings.setValue(QStringLiteral("nowPlaying/visible"), on);
}

void Application::quit() {
    if (quitting) {
        return;
    }
    quitting = true;
    saveState();
    corePlayer.shutDown();
    for (Ui::SkinnedWindow* window : windows()) {
        window->hide();
    }
    // Queued, so a quit() from inside a nested loop (a menu) lands in the main one.
    const auto finish = [] {
        QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
    };
    if (apiClient.pendingPosts() == 0) {
        return finish();
    }
    connect(&apiClient, &Yandex::ApiClient::postsSettled, this, finish);
    QTimer::singleShot(1500, this, finish);
}

void Application::saveState() {
    settings.setValue(QStringLiteral("volume"), mainWindowInstance->volume());
    settings.setValue(QStringLiteral("balance"), mainWindowInstance->balance());
    settings.setValue(QStringLiteral("vis/mode"), static_cast<int>(mainWindowInstance->visMode()));
    settings.setValue(QStringLiteral("time/remaining"), mainWindowInstance->showsRemainingTime());
    settings.setValue(QStringLiteral("equalizer/auto"), equalizerWindowInstance->autoOn());
    if (!Ui::SkinnedWindow::CanPositionWindows() || !mainWindowInstance->isVisible()
        || transientScale) {
        return;
    }
    settings.setValue(QStringLiteral("mainWindow/pos"), mainWindowInstance->pos());
    settings.setValue(QStringLiteral("equalizer/pos"), equalizerWindowInstance->pos());
    settings.setValue(QStringLiteral("playlist/pos"), playlistWindowInstance->pos());
    settings.setValue(QStringLiteral("nowPlaying/pos"), nowPlayingWindowInstance->pos());
    if (milkdropWindowInstance) {
        settings.setValue(QStringLiteral("milkdrop/pos"), milkdropWindowInstance->pos());
    }
    if (jamWindowInstance) {
        settings.setValue(QStringLiteral("jamWindow/pos"), jamWindowInstance->pos());
    }
}

void Application::installShortcuts(QWidget* widget) {
    auto add = [widget](const QKeySequence& shortcut, auto handler) {
        auto* action = new QAction(widget);
        action->setShortcut(shortcut);
        QObject::connect(action, &QAction::triggered, widget, handler);
        widget->addAction(action);
    };
    for (int key :
         {Qt::Key_Z, Qt::Key_X, Qt::Key_C, Qt::Key_V, Qt::Key_B, Qt::Key_Left, Qt::Key_Right}) {
        add(QKeySequence(key), [this, key] { transportKey(key); });
    }
    add(QKeySequence(Qt::ALT | Qt::Key_G),
        [this] { setEqualizerVisible(!equalizerWindowInstance->isVisible()); });
    add(QKeySequence(Qt::ALT | Qt::Key_E),
        [this] { setPlaylistVisible(!playlistWindowInstance->isVisible()); });
    add(QKeySequence(Qt::CTRL | Qt::Key_D),
        [this] { setScale(std::abs(mainWindowInstance->scale() - 2.0) < 1e-6 ? 1.0 : 2.0); });
    add(QKeySequence(Qt::CTRL | Qt::Key_W),
        [this] { mainWindowInstance->setShaded(!mainWindowInstance->isShaded()); });
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K), [this] {
        setMilkdropVisible(milkdropWindowInstance && !milkdropWindowInstance->isVisible());
    });
}

void Application::transportKey(int key) {
    switch (key) {
        case Qt::Key_Z: corePlayer.previous(); break;
        case Qt::Key_X: corePlayer.play(); break;
        case Qt::Key_C: corePlayer.pause(); break;
        case Qt::Key_V: corePlayer.stop(); break;
        case Qt::Key_B: corePlayer.next(); break;
        case Qt::Key_Left: corePlayer.seekTo(audioEngine.positionSeconds() - 5); break;
        case Qt::Key_Right: corePlayer.seekTo(audioEngine.positionSeconds() + 5); break;
        default: break;
    }
}

void Application::fillWindowActions(QMenu* menu) {
    QAction* equalizerAction =
        menu->addAction(tr("Equalizer"), this, [this](bool on) { setEqualizerVisible(on); });
    equalizerAction->setCheckable(true);
    equalizerAction->setChecked(equalizerWindowInstance->isVisible());
    equalizerAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_G));
    QAction* playlistAction =
        menu->addAction(tr("Playlist"), this, [this](bool on) { setPlaylistVisible(on); });
    playlistAction->setCheckable(true);
    playlistAction->setChecked(playlistWindowInstance->isVisible());
    playlistAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_E));
    QAction* nowPlayingAction =
        menu->addAction(tr("Now playing"), this, [this](bool on) { setNowPlayingVisible(on); });
    nowPlayingAction->setCheckable(true);
    nowPlayingAction->setChecked(nowPlayingWindowInstance->isVisible());
    if (milkdropWindowInstance) {
        QAction* milkdropAction =
            menu->addAction(QStringLiteral("Milkdrop"), this, [this](bool on) {
                setMilkdropVisible(on);
            });
        milkdropAction->setCheckable(true);
        milkdropAction->setChecked(milkdropWindowInstance->isVisible());
        milkdropAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K));
    }

    QMenu* visualizationMenu = menu->addMenu(tr("Visualization"));
    auto* visualizationGroup = new QActionGroup(visualizationMenu);
    const std::pair<Ui::MainWindow::VisMode, QString> modes[] = {
        {Ui::MainWindow::VisMode::Spectrum, tr("Spectrum")},
        {Ui::MainWindow::VisMode::Oscilloscope, tr("Oscilloscope")},
        {Ui::MainWindow::VisMode::Off, tr("Off")}
    };
    for (const auto& [mode, name] : modes) {
        QAction* action = visualizationMenu->addAction(name, this, [this, mode] {
            mainWindowInstance->setVisMode(mode);
            saveState();
        });
        action->setCheckable(true);
        action->setChecked(mainWindowInstance->visMode() == mode);
        visualizationGroup->addAction(action);
    }

    QMenu* shuffleMenu = menu->addMenu(tr("Shuffle"));
    shuffleMenu->setObjectName(QStringLiteral("shuffleMenu"));
    auto* shuffleGroup = new QActionGroup(shuffleMenu);
    const std::pair<Core::ShuffleAlgorithm, QString> algorithms[] = {
        {Core::ShuffleAlgorithm::Random, tr("Random track")},
        {Core::ShuffleAlgorithm::WithoutRepeats, tr("Without repeats")}
    };
    for (const auto& [algorithm, name] : algorithms) {
        QAction* action = shuffleMenu->addAction(name, this, [this, algorithm] {
            corePlayer.setShuffleAlgorithm(algorithm);
        });
        action->setCheckable(true);
        action->setChecked(corePlayer.shuffleAlgorithm() == algorithm);
        shuffleGroup->addAction(action);
    }

    QMenu* skinsMenu = menu->addMenu(tr("Skins"));
    const QStringList builtin = QDir(QStringLiteral(":/skins"))
                                    .entryList({QStringLiteral("*.wsz")}, QDir::Files, QDir::Name);
    for (const QString& name : builtin) {
        const QString path = QStringLiteral(":/skins/") + name;
        skinsMenu->addAction(QString(name).chopped(4), this, [this, path] { loadSkin(path); });
    }
    skinsMenu->addSeparator();
    skinsMenu->addAction(tr("Load a skin…"), this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            mainWindowInstance.get(), tr("Winamp skin"), QDir::homePath(),
            tr("Winamp skins (*.wsz *.zip)")
        );
        if (!path.isEmpty()) {
            loadSkin(path);
        }
    });

    QMenu* sizeMenu = menu->addMenu(tr("Size"));
    auto* sizeGroup = new QActionGroup(sizeMenu);
    for (double scaleFactor : {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0}) {
        QAction* action = sizeMenu->addAction(
            QStringLiteral("%1%").arg(qRound(scaleFactor * 100)), this,
            [this, scaleFactor] { setScale(scaleFactor); }
        );
        action->setCheckable(true);
        action->setChecked(std::abs(mainWindowInstance->scale() - scaleFactor) < 1e-6);
        sizeGroup->addAction(action);
    }
    sizeMenu->addSeparator();
    QAction* doubleSizeAction = sizeMenu->addAction(tr("Double size"), this, [this] {
        setScale(std::abs(mainWindowInstance->scale() - 2.0) < 1e-6 ? 1.0 : 2.0);
    });
    doubleSizeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));

    // Named in each language, and with the English word, so that a wrong pick can be undone.
    QString languageTitle = tr("Language");
    if (languageTitle != QLatin1String("Language")) {
        languageTitle += QStringLiteral(" (Language)");
    }
    QMenu* languageMenu = menu->addMenu(languageTitle);
    auto* languageGroup = new QActionGroup(languageMenu);
    for (Language language : Languages()) {
        QAction* action = languageMenu->addAction(LanguageName(language), this, [this, language] {
            setLanguage(language);
        });
        action->setCheckable(true);
        action->setChecked(language == translations.language());
        languageGroup->addAction(action);
    }

    QAction* alwaysOnTopAction =
        menu->addAction(tr("Always on top"), this, [this](bool on) { setAlwaysOnTop(on); });
    alwaysOnTopAction->setCheckable(true);
    alwaysOnTopAction->setChecked(
        mainWindowInstance->windowFlags().testFlag(Qt::WindowStaysOnTopHint)
    );
}

void Application::showSourcesMenu(QPoint globalPosition) {
    auto* menu = new QMenu(mainWindowInstance.get());
    menu->setAttribute(Qt::WA_DeleteOnClose);
    Ui::AddLibraryActions(
        menu, &corePlayer, &sources, corePlayer.currentTrack(), mainWindowInstance.get(),
        [this] { login(); }
    );
    menu->popup(globalPosition);
}

void Application::showMainMenu(QPoint globalPosition, const Yandex::Track* track) {
    auto* menu = new QMenu(mainWindowInstance.get());
    menu->setAttribute(Qt::WA_DeleteOnClose);
    Ui::AddLibraryActions(menu, &corePlayer, &sources, track, mainWindowInstance.get(), [this] {
        login();
    });
    menu->addSeparator();
    addJamMenu(menu);
    fillWindowActions(menu);
    menu->addSeparator();
    if (yandexLibrary.isLoggedIn()) {
        menu->addAction(
            tr("Log out (%1)").arg(yandexLibrary.account().displayName), this, &Application::logout
        );
    }
    menu->addAction(tr("Quit QiYaa"), this, &Application::quit);
    menu->popup(globalPosition);
}

void Application::addJamMenu(QMenu* menu) {
#ifdef QIYAA_HAVE_JAM
    QMenu* jam = menu->addMenu(tr("Jam"));
    const Jam::HostPhase phase = jamHostSession->phase();
    jam->addAction(phase == Jam::HostPhase::None ? tr("Start…") : tr("Jam window"), this, [this] {
        setJamWindowVisible(true);
    });
    jam->addAction(tr("Join…"))->setEnabled(false);  // Kickoman/QiYaa#16
    QAction* end = jam->addAction(tr("End"), this, [this] {
        QMessageBox ask(
            QMessageBox::Question, tr("End the jam"),
            tr("End the jam? The guests will see that it is over."), QMessageBox::NoButton,
            mainWindowInstance.get()
        );
        QPushButton* endIt = ask.addButton(tr("End it"), QMessageBox::DestructiveRole);
        ask.setDefaultButton(ask.addButton(tr("Cancel"), QMessageBox::RejectRole));
        ask.exec();
        if (ask.clickedButton() == endIt) {
            jamHostSession->end();
        }
    });
    end->setEnabled(phase == Jam::HostPhase::Active);
    jam->addSeparator();
    jam->addAction(tr("Server settings…"), this, &Application::showJamServerDialog);
    menu->addSeparator();
#else
    Q_UNUSED(menu);
#endif
}

void Application::showJamServerDialog() {
#ifdef QIYAA_HAVE_JAM
    Ui::JamServerDialog dialog(
        JamServer(settings), settings.value(QStringLiteral("jam/waveFeedback"), true).toBool(),
        settings.value(QStringLiteral("jam/shareAudio"), false).toBool(),
        QStringLiteral(QIYAA_JAM_URL), mainWindowInstance.get()
    );
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    settings.setValue(QStringLiteral("jam/server"), dialog.server());
    settings.setValue(QStringLiteral("jam/waveFeedback"), dialog.waveFeedback());
    settings.setValue(QStringLiteral("jam/shareAudio"), dialog.shareAudio());
    jamWindowInstance->setServerName(JamServerName(dialog.server()));
#endif
}

QImage Application::snapshot() const {
    QRect bounds;
    for (Ui::SkinnedWindow* window : windows()) {
        if (window->isVisible()) {
            bounds |= window->frameGeometry();
        }
    }
    QImage image(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    for (Ui::SkinnedWindow* window : windows()) {
        if (window->isVisible()) {
            painter.drawPixmap(window->pos() - bounds.topLeft(), window->grab());
        }
    }
    return image;
}

}  // namespace App
