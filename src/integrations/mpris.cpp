#include "integrations/mpris.h"

#include "core/player.h"
#include "integrations/media_controls.h"
#include "yandex/api_client.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QVariant>

#include <algorithm>
#include <cmath>

namespace Integrations {

namespace {
const QString kObjectPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");

QDBusObjectPath TrackPath(const QString& id) {
    // Object paths allow [A-Za-z0-9_] only.
    QString safe;
    for (QChar character : id) {
        safe += (character.isLetterOrNumber() && character.unicode() < 128) ? character : u'_';
    }
    return QDBusObjectPath(
        QStringLiteral("/io/github/kickoman/qiyaa/track/")
        + (safe.isEmpty() ? QStringLiteral("none") : safe)
    );
}
}  // namespace

Mpris::Mpris(
    MediaControls* controls,
    const QString& serviceSuffix,
    const QDBusConnection& bus,
    QObject* parent
)
    : QObject(parent)
    , mediaControls(controls)
    , connection(bus) {
    new MprisRootAdaptor(this);
    auto* playerAdaptor = new MprisPlayerAdaptor(this);

    if (!bus.isConnected()) {
        qInfo("MPRIS: no D-Bus session bus");
        return;
    }
    service = QStringLiteral("org.mpris.MediaPlayer2.") + serviceSuffix;
    if (connection.interface()->isServiceRegistered(service)) {
        service += QStringLiteral(".instance%1").arg(QCoreApplication::applicationPid());
    }
    if (!connection.registerObject(kObjectPath, this)) {
        qWarning("MPRIS: %s is taken on this connection", qPrintable(kObjectPath));
        return;
    }
    if (!connection.registerService(service)) {
        qWarning(
            "MPRIS: cannot register %s: %s", qPrintable(service),
            qPrintable(connection.lastError().message())
        );
        connection.unregisterObject(kObjectPath);
        return;
    }
    registered = true;

    connect(mediaControls, &MediaControls::trackChanged, this, [this, playerAdaptor] {
        emitPropertiesChanged(
            kPlayerInterface,
            {{QStringLiteral("Metadata"), metadata()},
             {QStringLiteral("CanSeek"), playerAdaptor->canSeek()}}
        );
    });
    connect(mediaControls, &MediaControls::artChanged, this, [this] {
        emitPropertiesChanged(kPlayerInterface, {{QStringLiteral("Metadata"), metadata()}});
    });
    connect(mediaControls, &MediaControls::statusChanged, this, [this, playerAdaptor] {
        emitPropertiesChanged(
            kPlayerInterface,
            {{QStringLiteral("PlaybackStatus"), playbackStatus()},
             {QStringLiteral("CanSeek"), playerAdaptor->canSeek()}}
        );
    });
    connect(mediaControls, &MediaControls::modesChanged, this, [this, playerAdaptor] {
        emitPropertiesChanged(
            kPlayerInterface,
            {{QStringLiteral("Shuffle"), playerAdaptor->shuffle()},
             {QStringLiteral("LoopStatus"), playerAdaptor->loopStatus()}}
        );
    });
    connect(mediaControls, &MediaControls::volumeChanged, this, [this, playerAdaptor] {
        emitPropertiesChanged(
            kPlayerInterface, {{QStringLiteral("Volume"), playerAdaptor->volume()}}
        );
    });
    connect(mediaControls, &MediaControls::seeked, playerAdaptor, [playerAdaptor](double seconds) {
        Q_EMIT playerAdaptor->Seeked(static_cast<qlonglong>(seconds * 1e6));
    });
}

Mpris::~Mpris() {
    if (!registered) {
        return;
    }
    connection.unregisterService(service);
    connection.unregisterObject(kObjectPath);
}

QString Mpris::playbackStatus() const {
    switch (mediaControls->status()) {
        case MediaControls::Status::Playing: return QStringLiteral("Playing");
        case MediaControls::Status::Paused: return QStringLiteral("Paused");
        case MediaControls::Status::Stopped: return QStringLiteral("Stopped");
    }
    return QStringLiteral("Stopped");
}

QVariantMap Mpris::metadata() const {
    const Yandex::Track* track = mediaControls->player()->currentTrack();
    if (!track) {
        return {
            {QStringLiteral("mpris:trackid"),
             QVariant::fromValue(
                 QDBusObjectPath(QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack"))
             )}
        };
    }
    QVariantMap fields{
        {QStringLiteral("mpris:trackid"), QVariant::fromValue(TrackPath(track->id))},
        {QStringLiteral("mpris:length"), static_cast<qlonglong>(track->durationMs) * 1000},
        {QStringLiteral("xesam:title"), track->title},
        {QStringLiteral("xesam:artist"), track->artists},
        {QStringLiteral("xesam:url"), track->webUrl().toString()},
    };
    if (!track->albumTitle.isEmpty()) {
        fields.insert(QStringLiteral("xesam:album"), track->albumTitle);
    }
    if (const QUrl artUrl = mediaControls->artUrl(); !artUrl.isEmpty()) {
        fields.insert(QStringLiteral("mpris:artUrl"), artUrl.toString());
    }
    return fields;
}

void Mpris::emitPropertiesChanged(const QString& interface, const QVariantMap& changed) {
    if (!registered) {
        return;
    }
    QDBusMessage message = QDBusMessage::createSignal(
        kObjectPath, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("PropertiesChanged")
    );
    message << interface << changed << QStringList();
    connection.send(message);
}

MprisRootAdaptor::MprisRootAdaptor(Mpris* parent)
    : QDBusAbstractAdaptor(parent)
    , mpris(parent) { }

void MprisRootAdaptor::Raise() {
    if (auto& raiseHook = mpris->controls()->hooks().raise) {
        raiseHook();
    }
}

void MprisRootAdaptor::Quit() {
    if (auto& quitHook = mpris->controls()->hooks().quit) {
        quitHook();
    }
}

MprisPlayerAdaptor::MprisPlayerAdaptor(Mpris* parent)
    : QDBusAbstractAdaptor(parent)
    , mpris(parent) { }

QString MprisPlayerAdaptor::playbackStatus() const {
    return mpris->playbackStatus();
}

QString MprisPlayerAdaptor::loopStatus() const {
    return mpris->controls()->player()->repeat() ? QStringLiteral("Playlist")
                                                 : QStringLiteral("None");
}

void MprisPlayerAdaptor::setLoopStatus(const QString& status) {
    mpris->controls()->player()->setRepeat(status != QLatin1String("None"));
}

bool MprisPlayerAdaptor::shuffle() const {
    return mpris->controls()->player()->shuffle();
}

void MprisPlayerAdaptor::setShuffle(bool on) {
    mpris->controls()->player()->setShuffle(on);
}

QVariantMap MprisPlayerAdaptor::metadata() const {
    return mpris->metadata();
}

double MprisPlayerAdaptor::volume() const {
    const auto& volumeHook = mpris->controls()->hooks().volume;
    return volumeHook ? volumeHook() / 100.0 : 1.0;
}

void MprisPlayerAdaptor::setVolume(double value) {
    if (const auto& setVolumeHook = mpris->controls()->hooks().setVolume) {
        setVolumeHook(static_cast<int>(std::lround(std::clamp(value, 0.0, 1.0) * 100)));
    }
}

qlonglong MprisPlayerAdaptor::position() const {
    return static_cast<qlonglong>(mpris->controls()->player()->engine()->positionSeconds() * 1e6);
}

bool MprisPlayerAdaptor::canSeek() const {
    return mpris->controls()->canSeek();
}

void MprisPlayerAdaptor::Next() {
    mpris->controls()->next();
}
void MprisPlayerAdaptor::Previous() {
    mpris->controls()->previous();
}
void MprisPlayerAdaptor::Pause() {
    mpris->controls()->pause();
}
void MprisPlayerAdaptor::PlayPause() {
    mpris->controls()->playPause();
}
void MprisPlayerAdaptor::Stop() {
    mpris->controls()->stop();
}
void MprisPlayerAdaptor::Play() {
    mpris->controls()->play();
}

void MprisPlayerAdaptor::Seek(qlonglong offsetUs) {
    if (!canSeek()) {
        return;
    }
    const double target = std::max(0.0, position() / 1e6 + offsetUs / 1e6);
    if (target >= mpris->controls()->player()->durationSeconds()) {
        return Next();
    }
    mpris->controls()->seekTo(target);
}

void MprisPlayerAdaptor::SetPosition(const QDBusObjectPath& trackId, qlonglong positionUs) {
    const auto* track = mpris->controls()->player()->currentTrack();
    if (!canSeek() || !track || trackId != TrackPath(track->id)) {
        return;
    }
    if (positionUs < 0 || positionUs > static_cast<qlonglong>(track->durationMs) * 1000) {
        return;
    }
    mpris->controls()->seekTo(positionUs / 1e6);
}

}  // namespace Integrations
