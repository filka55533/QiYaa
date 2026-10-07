#include "yandex/api_client.h"

#include "yandex/track_url.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

#include <optional>
#include <utility>

namespace Yandex {

namespace {
constexpr int kTimeoutMs = 20'000;

QNetworkRequest MakeRequest(const QUrl& url, const QString& token) {
    QNetworkRequest request(url);
    if (!token.isEmpty()) {
        request.setRawHeader("Authorization", "OAuth " + token.toUtf8());
    }
    request.setRawHeader("Accept-Language", "ru");
    request.setTransferTimeout(kTimeoutMs);
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy
    );
    return request;
}
}  // namespace

QString Track::displayTitle() const {
    return artists.isEmpty() ? title
                             : artists.join(QStringLiteral(", ")) + QStringLiteral(" - ") + title;
}

QUrl Track::coverUrl(int size) const {
    if (coverUri.isEmpty()) {
        return {};
    }
    QString uri = coverUri;
    uri.replace(QStringLiteral("%%"), QStringLiteral("%1x%1").arg(size));
    return QUrl(uri.startsWith(QLatin1String("http")) ? uri : QStringLiteral("https://") + uri);
}

QUrl Track::webUrl() const {
    if (albumId.isEmpty()) {
        return QUrl(QStringLiteral("https://music.yandex.ru/track/%1").arg(id));
    }
    return QUrl(QStringLiteral("https://music.yandex.ru/album/%1/track/%2").arg(albumId, id));
}

ApiClient::ApiClient(QNetworkAccessManager* networkAccessManager, QObject* parent)
    : QObject(parent)
    , networkManager(networkAccessManager)
    , baseUrl(QStringLiteral("https://api.music.yandex.net")) { }

QString ApiClient::IdString(const QJsonValue& value) {
    if (value.isDouble()) {
        return QString::number(static_cast<qint64>(value.toDouble()));
    }
    return value.toString();
}

void ApiClient::getJson(const QString& path, const QUrlQuery& query, TJsonCallback callback) {
    QUrl url(baseUrl + path);
    if (!query.isEmpty()) {
        url.setQuery(query);
    }
    handleJson(networkManager->get(MakeRequest(url, accessToken)), std::move(callback));
}

void ApiClient::getClassifiedJson(
    const QString& path,
    const QUrlQuery& query,
    TClassifiedJsonCallback callback
) {
    QUrl url(baseUrl + path);
    if (!query.isEmpty()) {
        url.setQuery(query);
    }
    handleClassifiedJson(networkManager->get(MakeRequest(url, accessToken)), std::move(callback));
}

void ApiClient::postForm(const QString& path, const TForm& form, TJsonCallback callback) {
    QByteArray body;
    for (const auto& [key, value] : form) {
        if (!body.isEmpty()) {
            body += '&';
        }
        body += QUrl::toPercentEncoding(key) + '=' + QUrl::toPercentEncoding(value);
    }
    QNetworkRequest request = MakeRequest(QUrl(baseUrl + path), accessToken);
    request.setHeader(
        QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded")
    );
    QNetworkReply* reply = networkManager->post(request, body);
    handleJson(reply, std::move(callback));
    trackPost(reply);
}

void ApiClient::postJson(const QString& path, const QJsonObject& body, TJsonCallback callback) {
    QNetworkRequest request = MakeRequest(QUrl(baseUrl + path), accessToken);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply =
        networkManager->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    handleJson(reply, std::move(callback));
    trackPost(reply);
}

void ApiClient::trackPost(QNetworkReply* reply) {
    ++pendingPostCount;
    // Connected after handleJson's slot, so this runs after the callback.
    connect(reply, &QNetworkReply::finished, this, [this] {
        if (--pendingPostCount == 0) {
            Q_EMIT postsSettled();
        }
    });
}

RequestError ClassifyReply(const QNetworkReply& reply) {
    RequestError error;
    error.httpStatus = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    switch (reply.error()) {
        case QNetworkReply::NoError: break;
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:  // also a body cut short after its status line
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::TimeoutError:
        case QNetworkReply::OperationCanceledError:  // the transfer timeout before Qt 6.11
        case QNetworkReply::SslHandshakeFailedError:  // captive portals
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        case QNetworkReply::BackgroundRequestNotAllowedError:
        case QNetworkReply::UnknownNetworkError:
        case QNetworkReply::ProxyConnectionRefusedError:
        case QNetworkReply::ProxyConnectionClosedError:
        case QNetworkReply::ProxyNotFoundError:
        case QNetworkReply::ProxyTimeoutError:
        case QNetworkReply::UnknownProxyError:
            error.kind = RequestError::Kind::Network;
            return error;
        default: error.kind = RequestError::Kind::Content; break;
    }
    if (error.httpStatus >= 400) {
        error.kind = RequestError::Kind::Http;
    }
    return error;
}

void ApiClient::handleJson(QNetworkReply* reply, TJsonCallback callback) {
    handleClassifiedJson(
        reply,
        [callback = std::move(callback)](const QJsonValue& result, const RequestError& error) {
            callback(result, error.text);
        }
    );
}

void ApiClient::handleClassifiedJson(QNetworkReply* reply, TClassifiedJsonCallback callback) {
    connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        RequestError error = ClassifyReply(*reply);
        const QJsonDocument document = QJsonDocument::fromJson(body);
        const QJsonObject object = document.object();

        if (error.isError()) {
            QString message = object.value(QStringLiteral("error"))
                                  .toObject()
                                  .value(QStringLiteral("message"))
                                  .toString();
            if (message.isEmpty()) {
                message = object.value(QStringLiteral("error")).toString();
            }
            if (message.isEmpty()) {
                message = reply->errorString();
            }
            error.text = QStringLiteral("HTTP %1 from %2: %3")
                             .arg(error.httpStatus)
                             .arg(reply->url().path(), message);
            callback({}, error);
            return;
        }
        if (!document.isObject()) {
            error.kind = RequestError::Kind::Content;
            error.text = QStringLiteral("%1: the %2-byte reply is not a JSON object")
                             .arg(reply->url().path())
                             .arg(body.size());
            callback({}, error);
            return;
        }
        callback(
            object.contains(QStringLiteral("result")) ? object.value(QStringLiteral("result"))
                                                      : QJsonValue(object),
            error
        );
    });
}

void ApiClient::accountStatus(TCallback<Account> callback) {
    getJson(
        QStringLiteral("/account/status"), {},
        [callback](const QJsonValue& result, const QString& error) {
            if (!error.isEmpty()) {
                return callback({}, error);
            }
            const QJsonObject accountObject =
                result.toObject().value(QStringLiteral("account")).toObject();
            Account account;
            account.uid = IdString(accountObject.value(QStringLiteral("uid")));
            account.login = accountObject.value(QStringLiteral("login")).toString();
            account.displayName = accountObject.value(QStringLiteral("displayName")).toString();
            if (account.displayName.isEmpty()) {
                account.displayName = account.login;
            }
            if (account.uid.isEmpty()) {
                return callback({}, QStringLiteral("not authorized (no uid) — token expired?"));
            }
            callback(account, {});
        }
    );
}

Track ApiClient::ParseTrack(const QJsonValue& value) {
    const QJsonObject object = value.toObject();
    Track track;
    track.id = IdString(object.value(QStringLiteral("id")));
    track.title = object.value(QStringLiteral("title")).toString();
    const QString version = object.value(QStringLiteral("version")).toString();
    if (!version.isEmpty()) {
        track.title += QStringLiteral(" (%1)").arg(version);
    }
    for (const QJsonValue& artist : object.value(QStringLiteral("artists")).toArray()) {
        track.artists << artist.toObject().value(QStringLiteral("name")).toString();
    }
    const QJsonArray albums = object.value(QStringLiteral("albums")).toArray();
    if (!albums.isEmpty()) {
        const QJsonObject album = albums.first().toObject();
        track.albumId = IdString(album.value(QStringLiteral("id")));
        track.albumTitle = album.value(QStringLiteral("title")).toString();
        track.year = album.value(QStringLiteral("year")).toInt();
        track.genre = album.value(QStringLiteral("genre")).toString();
        track.coverUri = album.value(QStringLiteral("coverUri")).toString();
    }
    if (track.coverUri.isEmpty()) {
        track.coverUri = object.value(QStringLiteral("coverUri")).toString();
    }
    if (track.coverUri.isEmpty()) {
        track.coverUri = object.value(QStringLiteral("ogImage")).toString();
    }
    track.durationMs = static_cast<qint64>(object.value(QStringLiteral("durationMs")).toDouble());
    track.available = object.value(QStringLiteral("available")).toBool(true);
    return track;
}

void ApiClient::tracks(const QStringList& ids, TCallback<QList<Track>> callback) {
    postForm(
        QStringLiteral("/tracks/"),
        {{QStringLiteral("track-ids"), ids.join(u',')},
         {QStringLiteral("with-positions"), QStringLiteral("false")}},
        [callback](const QJsonValue& result, const QString& error) {
            if (!error.isEmpty()) {
                return callback({}, error);
            }
            QList<Track> out;
            for (const QJsonValue& value : result.toArray()) {
                out << ParseTrack(value);
            }
            callback(out, {});
        }
    );
}

void ApiClient::resolveTrackUrl(const QString& trackId, TUrlCallback callback) {
    const QString id = trackId.section(u':', 0, 0);
    QPointer<ApiClient> self(this);
    QUrl url(baseUrl + QStringLiteral("/tracks/%1/download-info").arg(id));
    handleClassifiedJson(
        networkManager->get(MakeRequest(url, accessToken)),
        [self, callback, id](const QJsonValue& result, const RequestError& error) {
            if (!self) {
                return;
            }
            if (error.isError()) {
                return callback({}, error);
            }
            const QJsonArray variants = result.toArray();
            const std::optional<DownloadVariant> best =
                PickBestVariant(ParseDownloadVariants(variants));
            if (!best) {
                return callback(
                    {},
                    RequestError{
                        RequestError::Kind::Content, error.httpStatus,
                        QStringLiteral("track %1: none of %2 download variants has a usable link")
                            .arg(id)
                            .arg(variants.size())
                    }
                );
            }

            QUrl infoUrl = best->downloadInfoUrl;
            QUrlQuery query(infoUrl);
            query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
            infoUrl.setQuery(query);

            QNetworkReply* reply =
                self->networkManager->get(MakeRequest(infoUrl, self->accessToken));
            const int bitrate = best->bitrateKbps;
            connect(reply, &QNetworkReply::finished, self, [reply, callback, bitrate] {
                reply->deleteLater();
                RequestError infoError = ClassifyReply(*reply);
                if (infoError.isError()) {
                    infoError.text = QStringLiteral("download-info: ") + reply->errorString();
                    return callback({}, infoError);
                }
                const QByteArray body = reply->readAll();
                const std::optional<DownloadInfo> info = ParseDownloadInfo(body);
                if (!info) {
                    infoError.kind = RequestError::Kind::Content;
                    infoError.text =
                        QStringLiteral("download-info: no host, path and s in a %1-byte reply")
                            .arg(body.size());
                    return callback({}, infoError);
                }
                callback(ResolvedUrl{BuildTrackUrl(*info), bitrate}, {});
            });
        }
    );
}

void ApiClient::reportPlayStarted(
    const Account& account,
    const Track& track,
    const QString& playId
) {
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    postForm(
        QStringLiteral("/play-audio"),
        {{QStringLiteral("track-id"), track.id},
         {QStringLiteral("album-id"), track.albumId},
         {QStringLiteral("from"), QStringLiteral("web-own_tracks-track-track-main")},
         {QStringLiteral("play-id"), playId},
         {QStringLiteral("uid"), account.uid},
         {QStringLiteral("timestamp"), now},
         {QStringLiteral("client-now"), now},
         {QStringLiteral("track-length-seconds"), QString::number(track.durationMs / 1000.0)},
         {QStringLiteral("total-played-seconds"), QStringLiteral("0")},
         {QStringLiteral("end-position-seconds"), QStringLiteral("0")}},
        [](const QJsonValue&, const QString& error) {
            if (!error.isEmpty()) {
                qWarning("play-audio failed: %s", qPrintable(error));
            }
        }
    );
}

}  // namespace Yandex
