#include "ui/playlist_window.h"

#include "audio/audio_engine.h"
#include "core/player.h"
#include "skins/skin.h"
#include "skins/sprites.h"

#include <QApplication>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLatin1Char>
#include <QList>
#include <QMenu>
#include <QPainter>
#include <QPoint>
#include <QString>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Ui {

using Audio::AudioEngine;
using TSheet = Skins::Skin::Sheet;
using Skins::PlaylistSprites;

namespace {

constexpr int kListPaddingTop = 3;

bool RectContains(const QRect& rect, QPoint point) {
    return point.x() >= rect.x() && point.y() >= rect.y() && point.x() < rect.x() + rect.width()
        && point.y() < rect.y() + rect.height();
}

QString FormatTime(qint64 seconds) {
    seconds = std::max<qint64>(0, seconds);
    if (seconds >= 3600) {
        return QStringLiteral("%1:%2:%3")
            .arg(seconds / 3600)
            .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

constexpr int kMiniButtonX[] = {3, 11, 20, 29, 37, 45};
constexpr int kMiniButtonWidth = 8;
constexpr int kMiniButtonY = 22;
constexpr int kMiniButtonHeight = 10;

}  // namespace

PlaylistWindow::PlaylistWindow(Core::Player* player, const Skins::Skin* skin, QWidget* parent)
    : SkinnedWindow(
          skin,
          QSize(
              Skins::PlaylistSprites::kMinSize.width(),
              Skins::PlaylistSprites::kMinSize.height() + 4 * Skins::PlaylistSprites::kStepHeight
          ),
          parent
      )
    , corePlayer(player) {
    retranslate();
    setFocusPolicy(Qt::StrongFocus);
    connect(corePlayer, &Core::Player::queueReplaced, this, [this] {
        selectedRows.clear();
        anchor = cursorRow = -1;
        scrollRow = 0;
        update();
    });
    connect(corePlayer, &Core::Player::playlistChanged, this, [this] {
        const int trackCount = static_cast<int>(corePlayer->playlist().size());
        QSet<int> valid;
        for (int row : selectedRows) {
            if (row < trackCount) {
                valid.insert(row);
            }
        }
        selectedRows = valid;
        if (anchor >= trackCount) {
            anchor = trackCount - 1;
        }
        if (cursorRow >= trackCount) {
            cursorRow = trackCount - 1;
        }
        setScrollOffset(scrollRow);
        update();
    });
    connect(corePlayer, &Core::Player::positionTick, this, [this] {
        const int second = static_cast<int>(corePlayer->engine()->positionSeconds());
        if (second == shownSecond) {
            return;
        }
        shownSecond = second;
        const QSize size = skinSize();
        updateSkinRect(QRect(
            size.width() - 150 + 66, size.height() - Skins::PlaylistSprites::kBottomHeight + 23, 25,
            6
        ));
    });
    connect(corePlayer, &Core::Player::currentTrackChanged, this, [this] {
        ensureRowVisible(corePlayer->currentIndex());
        update();
    });
    connect(corePlayer->engine(), &Audio::AudioEngine::stateChanged, this, [this] { update(); });
}

void PlaylistWindow::setSizeSteps(QSize steps) {
    steps = steps.expandedTo(QSize(0, 0)).boundedTo(QSize(40, 40));
    if (steps == resizeSteps) {
        return;
    }
    resizeSteps = steps;
    const QSize fullSize = fullSkinSize();
    setSkinSize(isShaded() ? QSize(fullSize.width(), 14) : fullSize);
    setScrollOffset(scrollRow);
    Q_EMIT sizeStepsChanged(steps);
}

QRect PlaylistWindow::listRect() const {
    const QSize size = skinSize();
    return {
        Skins::PlaylistSprites::kLeftWidth, Skins::PlaylistSprites::kTopHeight,
        size.width() - Skins::PlaylistSprites::kLeftWidth - Skins::PlaylistSprites::kRightWidth,
        size.height() - Skins::PlaylistSprites::kTopHeight - Skins::PlaylistSprites::kBottomHeight
    };
}

int PlaylistWindow::visibleRows() const {
    return std::max(1, listRect().height() / Skins::PlaylistSprites::kRowHeight);
}

int PlaylistWindow::maxScroll() const {
    return std::max(0, static_cast<int>(corePlayer->playlist().size()) - visibleRows());
}

void PlaylistWindow::setScrollOffset(int row) {
    row = std::clamp(row, 0, maxScroll());
    if (row == scrollRow) {
        return;
    }
    scrollRow = row;
    update();
}

void PlaylistWindow::ensureRowVisible(int row) {
    if (row < 0) {
        return;
    }
    if (row < scrollRow) {
        setScrollOffset(row);
    } else if (row >= scrollRow + visibleRows()) {
        setScrollOffset(row - visibleRows() + 1);
    }
}

int PlaylistWindow::rowAt(QPoint skinPos) const {
    const QRect listArea = listRect();
    if (!RectContains(listArea, skinPos)) {
        return -1;
    }
    const int y = skinPos.y() - listArea.y() - kListPaddingTop;
    if (y < 0) {
        return -1;
    }
    const int rowOnScreen = y / Skins::PlaylistSprites::kRowHeight;
    if (rowOnScreen >= visibleRows()) {
        return -1;
    }
    const int row = scrollRow + rowOnScreen;
    return row < corePlayer->playlist().size() ? row : -1;
}

QRect PlaylistWindow::scrollHandleRect() const {
    const QRect listArea = listRect();
    const int travel =
        std::max(0, listArea.height() - Skins::PlaylistSprites::kScrollHandle.height());
    const int max = maxScroll();
    const int y = listArea.y()
        + (max > 0 ? static_cast<int>(std::lround(static_cast<double>(scrollRow) / max * travel))
                   : 0);
    return {
        skinSize().width() - 15, y, Skins::PlaylistSprites::kScrollHandle.width(),
        Skins::PlaylistSprites::kScrollHandle.height()
    };
}

void PlaylistWindow::drawTiles(QPainter& painter) const {
    const Skins::Skin& activeSkin = skin();
    const int windowWidth = skinSize().width(), windowHeight = skinSize().height();
    const bool active = isActiveWindow();

    for (int x = 25; x < windowWidth - 25; x += 25) {
        activeSkin.draw(
            painter, TSheet::PlEdit,
            active ? Skins::PlaylistSprites::kTopTileSelected : Skins::PlaylistSprites::kTopTile,
            {x, 0}
        );
    }
    activeSkin.draw(
        painter, TSheet::PlEdit,
        active ? Skins::PlaylistSprites::kTopLeftSelected : Skins::PlaylistSprites::kTopLeft, {0, 0}
    );
    activeSkin.draw(
        painter, TSheet::PlEdit,
        active ? Skins::PlaylistSprites::kTitleSelected : Skins::PlaylistSprites::kTitle,
        {(windowWidth - 100) / 2, 0}
    );
    activeSkin.draw(
        painter, TSheet::PlEdit,
        active ? Skins::PlaylistSprites::kTopRightSelected : Skins::PlaylistSprites::kTopRight,
        {windowWidth - 25, 0}
    );

    for (int y = Skins::PlaylistSprites::kTopHeight;
         y < windowHeight - Skins::PlaylistSprites::kBottomHeight; y += 29) {
        const int tileHeight =
            std::min(29, windowHeight - Skins::PlaylistSprites::kBottomHeight - y);
        activeSkin.draw(
            painter, TSheet::PlEdit,
            Skins::PlaylistSprites::kLeftTile.adjusted(0, 0, 0, tileHeight - 29), {0, y}
        );
        activeSkin.draw(
            painter, TSheet::PlEdit,
            Skins::PlaylistSprites::kRightTile.adjusted(0, 0, 0, tileHeight - 29),
            {windowWidth - Skins::PlaylistSprites::kRightWidth, y}
        );
    }

    for (int x = 125; x < windowWidth - 150; x += 25) {
        activeSkin.draw(
            painter, TSheet::PlEdit, Skins::PlaylistSprites::kBottomTile,
            {x, windowHeight - Skins::PlaylistSprites::kBottomHeight}
        );
    }
    activeSkin.draw(
        painter, TSheet::PlEdit, Skins::PlaylistSprites::kBottomLeft,
        {0, windowHeight - Skins::PlaylistSprites::kBottomHeight}
    );
    activeSkin.draw(
        painter, TSheet::PlEdit, Skins::PlaylistSprites::kBottomRight,
        {windowWidth - 150, windowHeight - Skins::PlaylistSprites::kBottomHeight}
    );

    activeSkin.draw(
        painter, TSheet::PlEdit,
        activeDrag == Drag::Scroll ? Skins::PlaylistSprites::kScrollHandleSelected
                                   : Skins::PlaylistSprites::kScrollHandle,
        scrollHandleRect().topLeft()
    );
    if (activeDrag == Drag::Close) {
        activeSkin.draw(
            painter, TSheet::PlEdit, Skins::PlaylistSprites::kCloseSelected, {windowWidth - 11, 3}
        );
    }
    if (activeDrag == Drag::Shade) {
        activeSkin.draw(
            painter, TSheet::PlEdit, Skins::PlaylistSprites::kCollapseSelected,
            {windowWidth - 21, 3}
        );
    }
}

void PlaylistWindow::drawRows(QPainter& painter) const {
    const Skins::Skin::PlaylistStyle& style = skin().playlistStyle();
    const QRect listArea = listRect();
    painter.fillRect(listArea, style.normalBackground);

    QFont font(style.font);
    font.setPixelSize(9);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
    painter.setFont(font);
    const QFontMetrics metrics(font);

    const auto& tracks = corePlayer->playlist();
    const int current = corePlayer->currentIndex();
    painter.save();
    painter.setClipRect(listArea);
    for (int i = 0; i < visibleRows(); ++i) {
        const int row = scrollRow + i;
        if (row >= tracks.size()) {
            break;
        }
        const QRect rowRect(
            listArea.x(), listArea.y() + kListPaddingTop + i * Skins::PlaylistSprites::kRowHeight,
            listArea.width(), Skins::PlaylistSprites::kRowHeight
        );
        if (selectedRows.contains(row)) {
            painter.fillRect(rowRect, style.selectedBackground);
        }
        painter.setPen(row == current ? style.current : style.normal);
        const QString duration = FormatTime(tracks[row].durationMs / 1000);
        const int durationWidth = metrics.horizontalAdvance(duration) + 3;
        const QString note = queueHooks.note ? queueHooks.note(row) : QString();
        const int noteWidth = note.isEmpty() ? 0 : metrics.horizontalAdvance(note) + 6;
        const QRect titleRect = rowRect.adjusted(2, 0, -durationWidth - 4 - noteWidth, 0);
        if (!note.isEmpty()) {
            painter.drawText(
                rowRect.adjusted(0, 0, -durationWidth - 4, 0), Qt::AlignRight | Qt::AlignVCenter,
                note
            );
        }
        const QString title = QStringLiteral("%1. %2").arg(row + 1).arg(tracks[row].displayTitle());
        painter.drawText(
            titleRect, Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(title, Qt::ElideRight, titleRect.width())
        );
        painter.drawText(
            rowRect.adjusted(0, 0, -3, 0), Qt::AlignRight | Qt::AlignVCenter, duration
        );
    }
    painter.restore();

    if (tracks.isEmpty()) {
        painter.setPen(style.normal);
        painter.drawText(
            listArea.adjusted(4, kListPaddingTop, -4, 0),
            Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
            tr("The playlist is empty.\nRight click or ADD: pick what to listen to.")
        );
    }
}

void PlaylistWindow::drawBottomInfo(QPainter& painter) const {
    const int windowWidth = skinSize().width(), windowHeight = skinSize().height();
    const QPoint base(windowWidth - 150, windowHeight - Skins::PlaylistSprites::kBottomHeight);

    qint64 totalSeconds = 0, selectedSeconds = 0;
    const auto& tracks = corePlayer->playlist();
    for (int i = 0; i < tracks.size(); ++i) {
        totalSeconds += tracks[i].durationMs / 1000;
        if (selectedRows.contains(i)) {
            selectedSeconds += tracks[i].durationMs / 1000;
        }
    }
    painter.save();
    painter.setClipRect(QRect(base + QPoint(7, 10), QSize(80, 6)));
    skin().drawText(
        painter, base + QPoint(7, 10), FormatTime(selectedSeconds) + u'/' + FormatTime(totalSeconds)
    );
    painter.restore();

    const auto state = corePlayer->engine()->state();
    if (state != Audio::AudioEngine::State::Stopped) {
        painter.save();
        painter.setClipRect(QRect(base + QPoint(66, 23), QSize(25, 6)));
        skin().drawText(
            painter, base + QPoint(66, 23),
            FormatTime(static_cast<qint64>(corePlayer->engine()->positionSeconds()))
                .rightJustified(5, u' ')
        );
        painter.restore();
    }
}

QSize PlaylistWindow::fullSkinSize() const {
    return {
        Skins::PlaylistSprites::kMinSize.width()
            + resizeSteps.width() * Skins::PlaylistSprites::kStepWidth,
        Skins::PlaylistSprites::kMinSize.height()
            + resizeSteps.height() * Skins::PlaylistSprites::kStepHeight
    };
}

void PlaylistWindow::setShaded(bool shaded) {
    if (shaded == isShaded()) {
        return;
    }
    const QSize fullSize = fullSkinSize();
    applyShade(shaded, shaded ? QSize(fullSize.width(), 14) : fullSize);
    setScrollOffset(scrollRow);
    update();
}

void PlaylistWindow::paintShaded(QPainter& painter) {
    const Skins::Skin& activeSkin = skin();
    const int windowWidth = skinSize().width();
    for (int x = 25; x < windowWidth - 50; x += 25) {
        activeSkin.draw(painter, TSheet::PlEdit, Skins::PlaylistSprites::kShadeTile, {x, 0});
    }
    activeSkin.draw(painter, TSheet::PlEdit, Skins::PlaylistSprites::kShadeLeft, {0, 0});
    activeSkin.draw(
        painter, TSheet::PlEdit,
        isActiveWindow() ? Skins::PlaylistSprites::kShadeRightSelected
                         : Skins::PlaylistSprites::kShadeRight,
        {windowWidth - 50, 0}
    );

    if (const auto* track = corePlayer->currentTrack()) {
        const QString time = FormatTime(track->durationMs / 1000);
        const int timeWidth = Skins::Skin::TextWidth(time);
        const int timeX = windowWidth - 30 - timeWidth;
        activeSkin.drawText(painter, {timeX, 4}, time);
        painter.save();
        painter.setClipRect(QRect(5, 4, timeX - 5 - 5, 6));
        activeSkin.drawText(
            painter, {5, 4},
            QStringLiteral("%1. %2").arg(corePlayer->currentIndex() + 1).arg(track->displayTitle()),
            timeX - 10
        );
        painter.restore();
    }
    if (activeDrag == Drag::Close) {
        activeSkin.draw(
            painter, TSheet::PlEdit, Skins::PlaylistSprites::kCloseSelected, {windowWidth - 11, 3}
        );
    }
    if (activeDrag == Drag::Shade) {
        activeSkin.draw(
            painter, TSheet::PlEdit, Skins::PlaylistSprites::kCollapseSelected,
            {windowWidth - 21, 3}
        );
    }
    if (activeDrag == Drag::Shade) {
        activeSkin.draw(
            painter, TSheet::PlEdit, Skins::PlaylistSprites::kExpandSelected, {windowWidth - 21, 3}
        );
    }
}

void PlaylistWindow::paintSkin(QPainter& painter) {
    if (isShaded()) {
        return paintShaded(painter);
    }
    drawRows(painter);
    drawTiles(painter);
    drawBottomInfo(painter);
}

PlaylistWindow::Button PlaylistWindow::buttonAt(QPoint point) const {
    constexpr Button kMiniButtons[] = {Button::Previous, Button::Play, Button::Pause,
                                       Button::Stop,     Button::Next, Button::Eject};
    constexpr Button kBottomButtons[] = {Button::Add, Button::Remove, Button::Select, Button::Misc};
    constexpr int kBottomButtonX[] = {14, 43, 72, 101};
    const int windowWidth = skinSize().width(), windowHeight = skinSize().height();
    const QPoint base(windowWidth - 150, windowHeight - Skins::PlaylistSprites::kBottomHeight);
    for (int i = 0; i < 6; ++i) {
        if (RectContains(
                QRect(
                    base + QPoint(kMiniButtonX[i], kMiniButtonY),
                    QSize(kMiniButtonWidth, kMiniButtonHeight)
                ),
                point
            )) {
            return kMiniButtons[i];
        }
    }
    const int y = windowHeight - 30;
    for (int i = 0; i < 4; ++i) {
        if (RectContains(QRect(kBottomButtonX[i], y, 22, 18), point)) {
            return kBottomButtons[i];
        }
    }
    if (RectContains(QRect(windowWidth - 44, y, 22, 18), point)) {
        return Button::List;
    }
    return Button::None;
}

bool PlaylistWindow::isDragArea(QPoint skinPos) const {
    return skinPos.y() < Skins::PlaylistSprites::kTopHeight;
}

void PlaylistWindow::selectRow(int row, Qt::KeyboardModifiers modifiers) {
    if (row < 0) {
        return;
    }
    if (modifiers & Qt::ShiftModifier && anchor >= 0) {
        selectedRows.clear();
        for (int i = std::min(anchor, row); i <= std::max(anchor, row); ++i) {
            selectedRows.insert(i);
        }
    } else if (modifiers & Qt::ControlModifier) {
        if (!selectedRows.remove(row)) {
            selectedRows.insert(row);
        }
        anchor = row;
    } else {
        selectedRows = {row};
        anchor = row;
    }
    cursorRow = row;
    update();
}

bool PlaylistWindow::skinMousePress(QPoint pos, Qt::MouseButton button) {
    if (button != Qt::LeftButton) {
        return false;
    }
    const int windowWidth = skinSize().width(), windowHeight = skinSize().height();
    dragStart = pos;

    if (RectContains(QRect(windowWidth - 11, 3, 9, 9), pos)) {
        activeDrag = Drag::Close;
        update();
        return true;
    }
    if (RectContains(QRect(windowWidth - 21, 3, 9, 9), pos)) {
        activeDrag = Drag::Shade;
        update();
        return true;
    }
    if (isShaded()) {
        return false;
    }
    if (RectContains(QRect(windowWidth - 20, windowHeight - 20, 20, 20), pos)) {
        activeDrag = Drag::Resize;
        dragStartSteps = resizeSteps;
        return true;
    }
    if (RectContains(
            QRect(
                windowWidth - Skins::PlaylistSprites::kRightWidth,
                Skins::PlaylistSprites::kTopHeight, Skins::PlaylistSprites::kRightWidth,
                windowHeight - Skins::PlaylistSprites::kTopHeight
                    - Skins::PlaylistSprites::kBottomHeight
            ),
            pos
        )) {
        activeDrag = Drag::Scroll;
        dragStartScroll = scrollRow;
        const QRect handle = scrollHandleRect();
        if (pos.y() < handle.y() || pos.y() >= handle.y() + handle.height()) {
            const QRect listArea = listRect();
            const double fraction =
                static_cast<double>(pos.y() - listArea.y() - handle.height() / 2)
                / std::max(1, listArea.height() - handle.height());
            setScrollOffset(
                static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * maxScroll()))
            );
            dragStartScroll = scrollRow;
        }
        update();
        return true;
    }
    if (const Button target = buttonAt(pos); target != Button::None) {
        activeDrag = Drag::Button;
        pressedButton = target;
        return true;
    }
    if (const int row = rowAt(pos); row >= 0) {
        selectRow(row, QApplication::keyboardModifiers());
        return true;
    }
    return false;
}

void PlaylistWindow::skinMouseMove(QPoint pos) {
    switch (activeDrag) {
        case Drag::Resize: {
            const QPoint delta = pos - dragStart;
            setSizeSteps(QSize(
                dragStartSteps.width()
                    + static_cast<int>(std::lround(
                        static_cast<double>(delta.x()) / Skins::PlaylistSprites::kStepWidth
                    )),
                dragStartSteps.height()
                    + static_cast<int>(std::lround(
                        static_cast<double>(delta.y()) / Skins::PlaylistSprites::kStepHeight
                    ))
            ));
            break;
        }
        case Drag::Scroll: {
            const int travel =
                std::max(1, listRect().height() - Skins::PlaylistSprites::kScrollHandle.height());
            const int dy = pos.y() - dragStart.y();
            setScrollOffset(
                dragStartScroll
                + static_cast<int>(std::lround(static_cast<double>(dy) / travel * maxScroll()))
            );
            break;
        }
        default: break;
    }
}

void PlaylistWindow::skinMouseRelease(QPoint pos, Qt::MouseButton button) {
    if (button != Qt::LeftButton) {
        return;
    }
    const Drag drag = activeDrag;
    activeDrag = Drag::None;
    const int windowWidth = skinSize().width();
    if (drag == Drag::Close && RectContains(QRect(windowWidth - 11, 3, 9, 9), pos)) {
        Q_EMIT closeRequested();
    }
    if (drag == Drag::Shade && RectContains(QRect(windowWidth - 21, 3, 9, 9), pos)) {
        setShaded(!isShaded());
    }
    if (drag == Drag::Button && buttonAt(pos) == pressedButton) {
        switch (pressedButton) {
            case Button::Add:
                Q_EMIT sourcesMenuRequested(mapToGlobal(
                    QPoint(qRound(14 * scale()), qRound((skinSize().height() - 30) * scale()))
                ));
                break;
            case Button::Remove: {
                auto* menu = new QMenu(this);
                menu->addAction(
                        tr("Remove the selected"), this, [this] { removeSelected(); }
                )->setEnabled(!selectedRows.isEmpty());
                menu->addAction(tr("Clear the playlist"), this, [this] { clearQueue(); });
                popupAt(menu, {43, skinSize().height() - 30});
                break;
            }
            case Button::Select: {
                auto* menu = new QMenu(this);
                menu->addAction(tr("Select all"), this, [this] {
                    selectedRows.clear();
                    for (int i = 0; i < corePlayer->playlist().size(); ++i) {
                        selectedRows.insert(i);
                    }
                    update();
                });
                menu->addAction(tr("Select none"), this, [this] {
                    selectedRows.clear();
                    update();
                });
                popupAt(menu, {72, skinSize().height() - 30});
                break;
            }
            case Button::Previous: corePlayer->previous(); break;
            case Button::Play: corePlayer->play(); break;
            case Button::Pause: corePlayer->pause(); break;
            case Button::Stop: corePlayer->stop(); break;
            case Button::Next: corePlayer->next(); break;
            default: break;
        }
    }
    pressedButton = Button::None;
    update();
}

bool PlaylistWindow::skinMouseDoubleClick(QPoint pos, Qt::MouseButton button) {
    if (button == Qt::LeftButton && (isShaded() || pos.y() < Skins::PlaylistSprites::kTopHeight)
        && pos.x() < skinSize().width() - 21) {
        setShaded(!isShaded());
        return true;
    }
    const int row = rowAt(pos);
    if (button != Qt::LeftButton || row < 0) {
        return false;
    }
    corePlayer->playIndex(row);
    return true;
}

void PlaylistWindow::wheelEvent(QWheelEvent* event) {
    const int steps = wheelSteps(event);
    if (steps != 0) {
        setScrollOffset(scrollRow - steps * 3);
    }
}

void PlaylistWindow::keyPressEvent(QKeyEvent* event) {
    const int count = static_cast<int>(corePlayer->playlist().size());
    if (count == 0) {
        return QWidget::keyPressEvent(event);
    }
    int targetRow =
        cursorRow >= 0 ? std::min(cursorRow, count - 1) : std::max(0, corePlayer->currentIndex());
    switch (event->key()) {
        case Qt::Key_Up: targetRow = std::max(0, targetRow - 1); break;
        case Qt::Key_Down: targetRow = std::min(count - 1, targetRow + 1); break;
        case Qt::Key_PageUp: targetRow = std::max(0, targetRow - visibleRows()); break;
        case Qt::Key_PageDown: targetRow = std::min(count - 1, targetRow + visibleRows()); break;
        case Qt::Key_Home: targetRow = 0; break;
        case Qt::Key_End: targetRow = count - 1; break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (cursorRow >= 0) {
                corePlayer->playIndex(cursorRow);
            }
            return;
        case Qt::Key_Delete: removeSelected(); return;
        default: return QWidget::keyPressEvent(event);
    }
    selectRow(targetRow, event->modifiers() & Qt::ShiftModifier);
    ensureRowVisible(targetRow);
}

void PlaylistWindow::setQueueHooks(QueueHooks hooks) {
    queueHooks = std::move(hooks);
    update();
}

void PlaylistWindow::removeSelected() {
    QList<int> rows = selectedRows.values();
    std::sort(rows.begin(), rows.end());
    if (!queueHooks.remove || !queueHooks.remove(rows)) {
        corePlayer->removeTracks(rows);
    }
    selectedRows.clear();
    update();
}

void PlaylistWindow::clearQueue() {
    if (!queueHooks.clear || !queueHooks.clear()) {
        corePlayer->clearQueue();
    }
}

void PlaylistWindow::popupAt(QMenu* menu, QPoint skinPos) {
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(mapToGlobal(QPoint(qRound(skinPos.x() * scale()), qRound(skinPos.y() * scale()))));
}

void PlaylistWindow::contextMenuEvent(QContextMenuEvent* event) {
    const int row = event->reason() == QContextMenuEvent::Keyboard
        ? (cursorRow >= 0 ? cursorRow : corePlayer->currentIndex())
        : rowAt(toSkin(event->pos()));
    if (row < 0) {
        Q_EMIT sourcesMenuRequested(event->globalPos());
        return;
    }
    if (!selectedRows.contains(row)) {
        selectRow(row, {});
    }
    Q_EMIT trackMenuRequested(event->globalPos(), row);
}

void PlaylistWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    Q_EMIT closeRequested();
}

void PlaylistWindow::retranslate() {
    setWindowTitle(tr("QiYaa: playlist"));
}

}  // namespace Ui
