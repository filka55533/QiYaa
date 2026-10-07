#include "ui/now_playing_window.h"

#include "core/cover_cache.h"
#include "core/player.h"
#include "skins/skin.h"
#include "yandex/library.h"

#include <QColor>
#include <QDesktopServices>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QLatin1Char>
#include <QPainter>
#include <QPoint>
#include <QString>
#include <QWidget>

#include <algorithm>

namespace Ui {

namespace {
constexpr int kPadding = 4;
constexpr int kCoverPixels = 400;
}  // namespace

NowPlayingWindow::NowPlayingWindow(
    Core::Player* player,
    Core::CoverCache* covers,
    const Skins::Skin* skin,
    QWidget* parent
)
    : GenWindow(skin, QStringLiteral("NOW PLAYING"), parent)
    , corePlayer(player)
    , coverCache(covers) {
    retranslate();
    connect(corePlayer, &Core::Player::currentTrackChanged, this, [this] { update(); });
    connect(corePlayer->library(), &Yandex::Library::likesChanged, this, [this] { update(); });
    connect(coverCache, &Core::CoverCache::ready, this, [this](const QUrl& url) {
        if (const auto* track = corePlayer->currentTrack();
            track && track->coverUrl(kCoverPixels) == url) {
            update();
        }
    });
}

QRect NowPlayingWindow::coverRect() const {
    const QRect inset = contentRect().adjusted(kPadding, kPadding, -kPadding, -kPadding);
    const int side = std::max(0, std::min(inset.height(), inset.width() / 2));
    return {inset.x(), inset.y(), side, side};
}

void NowPlayingWindow::paintContent(QPainter& painter, const QRect& area) {
    const Skins::Skin::PlaylistStyle& style = skin().playlistStyle();
    painter.fillRect(area, style.normalBackground);
    const Yandex::Track* track = corePlayer->currentTrack();

    const QRect cover = coverRect();
    const QImage image = track ? coverCache->get(track->coverUrl(kCoverPixels)) : QImage();
    if (!image.isNull()) {
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(cover, image);
        painter.restore();
    } else {
        painter.setPen(style.normal);
        painter.drawRect(cover.adjusted(0, 0, -1, -1));
    }

    const QRect textRect(
        cover.right() + 1 + kPadding * 2, cover.y(), area.right() - cover.right() - kPadding * 3,
        cover.height()
    );
    QFont font(style.font);
    font.setPixelSize(9);
    QFont bold = font;
    bold.setBold(true);
    bold.setPixelSize(11);
    int y = textRect.y();
    auto line = [&](const QFont& lineFont, const QColor& color, const QString& content) {
        if (content.isEmpty()) {
            return;
        }
        const QFontMetrics metrics(lineFont);
        if (y + metrics.height() > textRect.bottom() + 1) {
            return;
        }
        painter.setFont(lineFont);
        painter.setPen(color);
        painter.drawText(
            QRect(textRect.x(), y, textRect.width(), metrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(content, Qt::ElideRight, textRect.width())
        );
        y += metrics.height() + 1;
    };
    if (!track) {
        line(font, style.normal, tr("Nothing is playing"));
        return;
    }
    line(bold, style.current, track->title);
    line(font, style.normal, track->artists.join(QStringLiteral(", ")));
    y += 3;
    QString album = track->albumTitle;
    if (track->year > 0) {
        album += album.isEmpty() ? QString::number(track->year)
                                 : QStringLiteral(" (%1)").arg(track->year);
    }
    line(font, style.normal, album);
    const int seconds = static_cast<int>(track->durationMs / 1000);
    line(
        font, style.normal,
        QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'))
    );
    if (corePlayer->library()->isLiked(track->id)) {
        line(font, style.current, tr("♥ In Liked"));
    }
}

bool NowPlayingWindow::contentMousePress(QPoint pos, Qt::MouseButton button) {
    const Yandex::Track* track = corePlayer->currentTrack();
    if (button != Qt::LeftButton || !track || !coverRect().contains(pos)) {
        return false;
    }
    QDesktopServices::openUrl(track->webUrl());
    return true;
}

void NowPlayingWindow::retranslate() {
    setWindowTitle(tr("QiYaa: now playing"));
}

}  // namespace Ui
