#include "support/yandex_json.h"

#include "support/spec_fixtures.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QUrl>

namespace Tests {

QJsonObject ToJson(const Yandex::Account& account) {
    return {
        {QStringLiteral("uid"), account.uid},
        {QStringLiteral("login"), account.login},
        {QStringLiteral("displayName"), account.displayName}
    };
}

QJsonObject ToJson(const Yandex::Track& track) {
    return {
        {QStringLiteral("id"), track.id},
        {QStringLiteral("albumId"), track.albumId},
        {QStringLiteral("title"), track.title},
        {QStringLiteral("artists"), QJsonArray::fromStringList(track.artists)},
        {QStringLiteral("durationMs"), static_cast<double>(track.durationMs)},
        {QStringLiteral("available"), track.available},
        {QStringLiteral("albumTitle"), track.albumTitle},
        {QStringLiteral("year"), track.year},
        {QStringLiteral("genre"), track.genre},
        {QStringLiteral("coverUri"), track.coverUri}
    };
}

QJsonObject ToJson(const Yandex::WaveBatch& batch) {
    QJsonObject object = TracksJson(batch.tracks);
    object.insert(QStringLiteral("sessionId"), batch.sessionId);
    object.insert(QStringLiteral("batchId"), batch.batchId);
    return object;
}

QJsonObject ToJson(const Yandex::SearchResult& result) {
    using Kind = Yandex::SearchResult::Kind;
    QString type;
    switch (result.bestKind) {
        case Kind::None: break;
        case Kind::Artist: type = QStringLiteral("artist"); break;
        case Kind::Album: type = QStringLiteral("album"); break;
        case Kind::Track: type = QStringLiteral("track"); break;
        case Kind::Playlist: type = QStringLiteral("playlist"); break;
        case Kind::Other: type = QStringLiteral("other"); break;
    }
    QJsonObject object = TracksJson(result.tracks);
    object.insert(QStringLiteral("bestType"), type);
    object.insert(QStringLiteral("bestId"), result.bestId);
    object.insert(QStringLiteral("bestName"), result.bestName);
    return object;
}

QJsonObject ToJson(const Yandex::DownloadInfo& info) {
    return {
        {QStringLiteral("host"), info.host},
        {QStringLiteral("path"), info.path},
        {QStringLiteral("ts"), info.ts},
        {QStringLiteral("s"), info.s},
        {QStringLiteral("trackUrl"), Yandex::BuildTrackUrl(info).toString()}
    };
}

QJsonObject TracksJson(const QList<Yandex::Track>& tracks) {
    QJsonArray array;
    for (const Yandex::Track& track : tracks) {
        array.append(ToJson(track));
    }
    return {{QStringLiteral("tracks"), array}};
}

QJsonObject IdsJson(const QString& key, const QStringList& ids) {
    return {{key, QJsonArray::fromStringList(ids)}};
}

QJsonObject PlaylistsJson(const QList<Yandex::PlaylistReference>& playlists) {
    QJsonArray array;
    for (const Yandex::PlaylistReference& playlist : playlists) {
        array.append(QJsonObject{
            {QStringLiteral("ownerUid"), playlist.ownerUid},
            {QStringLiteral("kind"), playlist.kind},
            {QStringLiteral("title"), playlist.title},
            {QStringLiteral("trackCount"), playlist.trackCount}
        });
    }
    return {{QStringLiteral("playlists"), array}};
}

QJsonObject NamedJson(const QString& key, const QList<Yandex::NamedReference>& references) {
    QJsonArray array;
    for (const Yandex::NamedReference& reference : references) {
        array.append(QJsonObject{
            {QStringLiteral("id"), reference.id}, {QStringLiteral("name"), reference.name}
        });
    }
    return {{key, array}};
}

QJsonObject StationsJson(const QList<Yandex::Station>& stations) {
    QJsonArray array;
    for (const Yandex::Station& station : stations) {
        array.append(QJsonObject{
            {QStringLiteral("id"), station.id},
            {QStringLiteral("type"), station.type},
            {QStringLiteral("name"), station.name}
        });
    }
    return {{QStringLiteral("stations"), array}};
}

QJsonObject WavesJson(const QList<Yandex::Wave>& waves) {
    QJsonArray array;
    for (const Yandex::Wave& wave : waves) {
        array.append(QJsonObject{
            {QStringLiteral("name"), wave.name},
            {QStringLiteral("description"), wave.description},
            {QStringLiteral("seeds"), QJsonArray::fromStringList(wave.seeds)}
        });
    }
    return {{QStringLiteral("waves"), array}};
}

QJsonObject VariantsJson(
    const QList<Yandex::DownloadVariant>& variants,
    const std::optional<Yandex::DownloadVariant>& best
) {
    QJsonArray array;
    for (const Yandex::DownloadVariant& variant : variants) {
        array.append(QJsonObject{
            {QStringLiteral("codec"), variant.codec},
            {QStringLiteral("bitrateKbps"), variant.bitrateKbps},
            {QStringLiteral("preview"), variant.preview},
            {QStringLiteral("downloadInfoUrl"), variant.downloadInfoUrl.toString()}
        });
    }
    return {
        {QStringLiteral("variants"), array},
        {QStringLiteral("best"),
         best ? QJsonValue(best->downloadInfoUrl.toString()) : QJsonValue(QJsonValue::Null)}
    };
}

QString CheckError(const QString& error, const QString& expectedName) {
    const QJsonObject expected =
        ExpectedObject(expectedName).value(QStringLiteral("error")).toObject();
    const QString status =
        QStringLiteral("HTTP %1").arg(expected.value(QStringLiteral("status")).toInt());
    if (!error.contains(status)) {
        return QStringLiteral("\"%1\" does not name %2").arg(error, status);
    }
    const QJsonValue message = expected.value(QStringLiteral("message"));
    if (!message.isNull() && !error.contains(message.toString())) {
        return QStringLiteral("\"%1\" does not contain \"%2\"").arg(error, message.toString());
    }
    return {};
}

}  // namespace Tests
