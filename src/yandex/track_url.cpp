#include "yandex/track_url.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace Yandex {

namespace {
constexpr char kSignSalt[] = "XGRlBW9FXlekgbPrRHuSiA";
}  // namespace

QList<DownloadVariant> ParseDownloadVariants(const QJsonArray& result) {
    QList<DownloadVariant> out;
    for (const QJsonValue& value : result) {
        const QJsonObject object = value.toObject();
        DownloadVariant variant;
        variant.codec = object.value(QStringLiteral("codec")).toString();
        variant.bitrateKbps = object.value(QStringLiteral("bitrateInKbps")).toInt();
        variant.preview = object.value(QStringLiteral("preview")).toBool();
        variant.downloadInfoUrl = QUrl(object.value(QStringLiteral("downloadInfoUrl")).toString());
        if (variant.downloadInfoUrl.isValid()) {
            out.append(variant);
        }
    }
    return out;
}

std::optional<DownloadVariant> PickBestVariant(const QList<DownloadVariant>& variants) {
    const DownloadVariant* best = nullptr;
    for (const DownloadVariant& variant : variants) {
        if (variant.codec != QLatin1String("mp3") || variant.preview) {
            continue;
        }
        if (!best || variant.bitrateKbps > best->bitrateKbps) {
            best = &variant;
        }
    }
    if (!best && !variants.isEmpty()) {
        best = &variants.first();
    }
    if (!best) {
        return std::nullopt;
    }
    return *best;
}

std::optional<DownloadInfo> ParseDownloadInfo(const QByteArray& json) {
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    auto text = [&](const char* key) {
        const QJsonValue value = object.value(QLatin1String(key));
        return value.isString() ? value.toString()
            : value.isDouble()  ? QString::number(static_cast<qint64>(value.toDouble()))
                                : QString();
    };
    DownloadInfo info;
    info.host = text("host");
    info.path = text("path");
    info.ts = text("ts");
    info.s = text("s");
    if (info.host.isEmpty() || !info.path.startsWith(u'/') || info.s.isEmpty()) {
        return std::nullopt;
    }
    return info;
}

QUrl BuildTrackUrl(const DownloadInfo& info) {
    const QByteArray toSign = QByteArray(kSignSalt) + info.path.mid(1).toUtf8() + info.s.toUtf8();
    const QByteArray signature = QCryptographicHash::hash(toSign, QCryptographicHash::Md5).toHex();
    return QUrl(QStringLiteral("https://%1/get-mp3/%2/%3%4")
                    .arg(info.host, QString::fromLatin1(signature), info.ts, info.path));
}

}  // namespace Yandex
