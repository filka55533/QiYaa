#include "ui/skinned_window.h"

#include "skins/region.h"
#include "skins/skin.h"
#include "ui/snap.h"

#include <QEvent>
#include <QGuiApplication>
#include <QLatin1String>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QScreen>
#include <QTransform>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace Ui {

namespace {
QList<SkinnedWindow*>& WindowRegistry() {
    static QList<SkinnedWindow*> windows;
    return windows;
}

QList<QRect> ScreenRects() {
    QList<QRect> rects;
    for (QScreen* targetScreen : QGuiApplication::screens()) {
        rects << targetScreen->availableGeometry();
    }
    return rects;
}
}  // namespace

bool SkinnedWindow::IsIntegerScale(double scale) {
    return std::abs(scale - std::round(scale)) < 1e-6;
}

SkinnedWindow::SkinnedWindow(const Skins::Skin* skin, QSize skinSize, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint)
    , currentSkin(skin)
    , skinPixelSize(skinSize) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    applySize();
    WindowRegistry().append(this);

    auto recheck = [this] {
        if (isVisible()) {
            ensureVisible();
        }
    };
    connect(qApp, &QGuiApplication::screenRemoved, this, recheck);
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen* addedScreen) {
        connect(addedScreen, &QScreen::availableGeometryChanged, this, [this] {
            if (isVisible()) {
                ensureVisible();
            }
        });
    });
    for (QScreen* targetScreen : QGuiApplication::screens()) {
        connect(targetScreen, &QScreen::availableGeometryChanged, this, recheck);
    }
}

SkinnedWindow::~SkinnedWindow() {
    WindowRegistry().removeAll(this);
}

const QList<SkinnedWindow*>& SkinnedWindow::AllWindows() {
    return WindowRegistry();
}

bool SkinnedWindow::CanPositionWindows() {
    return !QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}

void SkinnedWindow::setSecondary() {
#if defined(Q_OS_WIN)
    setWindowFlag(Qt::Tool, true);
#endif
}

void SkinnedWindow::setSkin(const Skins::Skin* skin) {
    currentSkin = skin;
    applyMask();
    skinChanged();
    update();
}

void SkinnedWindow::applySize() {
    setFixedSize(QSize(
        qRound(skinPixelSize.width() * scaleFactor), qRound(skinPixelSize.height() * scaleFactor)
    ));
    buffer = QImage();
}

void SkinnedWindow::setScale(double scale) {
    scale = std::round(std::clamp(scale, 1.0, 4.0) * 20.0) / 20.0;
    if (std::abs(scale - scaleFactor) < 1e-6) {
        return;
    }
    scaleFactor = scale;
    applySize();
    applyMask();
    ensureVisible();
    update();
}

void SkinnedWindow::setSkinSize(QSize size) {
    if (size == skinPixelSize) {
        return;
    }
    skinPixelSize = size;
    applySize();
    applyMask();
    update();
}

void SkinnedWindow::placeAt(QPoint pos) {
    if (!CanPositionWindows()) {
        return;
    }
    QRect rect(pos, size());
    move(Ui::ClampInside(rect, Ui::PickScreen(rect, ScreenRects())));
}

void SkinnedWindow::ensureVisible() {
    placeAt(pos());
}

void SkinnedWindow::applyShade(bool shaded, QSize newSkinSize) {
    shadeEnabled = shaded;
    resizeKeepingStack(newSkinSize);
    applyMask();  // the region section changes with the mode even if the size doesn't
    Q_EMIT shadeChanged(shaded);
}

void SkinnedWindow::resizeKeepingStack(QSize newSkinSize) {
    QList<SkinnedWindow*> all;
    QList<QRect> rects;
    QList<bool> visible;
    int self = -1;
    for (SkinnedWindow* window : WindowRegistry()) {
        if (window == this) {
            self = static_cast<int>(all.size());
        }
        all << window;
        rects << window->frameGeometry();
        visible << window->isVisible();
    }
    const int oldHeight = height();
    setSkinSize(newSkinSize);
    const int dy = height() - oldHeight;
    if (dy == 0 || !CanPositionWindows()) {
        return;
    }
    QList<SkinnedWindow*> group{this};
    for (int i : Ui::StackBelow(self, rects, dy, visible)) {
        all[i]->move(all[i]->pos() + QPoint(0, dy));
        group << all[i];
    }
    if (dy < 0 || !isVisible()) {
        return;
    }
    for (SkinnedWindow* window : dockedWindows()) {
        if (!group.contains(window)) {
            group << window;
        }
    }
    QRect bounds;
    for (SkinnedWindow* window : group) {
        if (window->isVisible()) {
            bounds |= window->frameGeometry();
        }
    }
    const QPoint shift =
        Ui::ClampInside(bounds, Ui::PickScreen(bounds, ScreenRects())) - bounds.topLeft();
    if (!shift.isNull()) {
        for (SkinnedWindow* window : group) {
            window->move(window->pos() + shift);
        }
    }
}

QList<SkinnedWindow*> SkinnedWindow::dockedWindows() const {
    QList<SkinnedWindow*> visible;
    QList<QRect> rects;
    int self = -1;
    for (SkinnedWindow* window : WindowRegistry()) {
        if (!window->isVisible()) {
            continue;
        }
        if (window == this) {
            self = static_cast<int>(visible.size());
        }
        visible << window;
        rects << window->frameGeometry();
    }
    QList<SkinnedWindow*> docked;
    for (int i : Ui::ConnectedGroup(self, rects)) {
        docked << visible[i];
    }
    return docked;
}

QPoint SkinnedWindow::toSkin(QPointF widgetPos) const {
    return QPoint(
        static_cast<int>(std::floor(widgetPos.x() / scaleFactor)),
        static_cast<int>(std::floor(widgetPos.y() / scaleFactor))
    );
}

int SkinnedWindow::wheelSteps(QWheelEvent* event) {
    wheelRemainder += event->angleDelta().y();
    const int steps = wheelRemainder / 120;
    wheelRemainder -= steps * 120;
    return steps;
}

void SkinnedWindow::updateSkinRect(const QRect& skinRect) {
    const QRectF scaled(
        skinRect.x() * scaleFactor, skinRect.y() * scaleFactor, skinRect.width() * scaleFactor,
        skinRect.height() * scaleFactor
    );
    update(scaled.toAlignedRect().adjusted(-1, -1, 1, 1));
}

void SkinnedWindow::applyMask() {
    const QString section = regionSection();
    const auto it =
        section.isEmpty() ? currentSkin->region().cend() : currentSkin->region().constFind(section);
    if (it == currentSkin->region().cend()) {
        clearMask();
        return;
    }
    const QTransform transform = QTransform::fromScale(scaleFactor, scaleFactor);
    QList<QPolygon> scaled;
    for (const QPolygon& polygon : *it) {
        scaled << transform.map(QPolygonF(polygon)).toPolygon();
    }
    setMask(Skins::RegionFromPolygons(scaled));
}

void SkinnedWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    if (IsIntegerScale(scaleFactor)) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.scale(scaleFactor, scaleFactor);
        paintSkin(painter);
        return;
    }
    const int bufferScale = static_cast<int>(std::ceil(scaleFactor * devicePixelRatioF() - 1e-6));
    const QSize bufferSize = skinPixelSize * bufferScale;
    if (buffer.size() != bufferSize) {
        buffer = QImage(bufferSize, QImage::Format_ARGB32_Premultiplied);
    }
    {
        QPainter bufferPainter(&buffer);
        bufferPainter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        bufferPainter.scale(bufferScale, bufferScale);
        paintSkin(bufferPainter);
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(rect(), buffer);
}

void SkinnedWindow::mousePressEvent(QMouseEvent* event) {
    const QPoint skinPos = toSkin(event->position());
    if (skinMousePress(skinPos, event->button())) {
        return;
    }
    if (event->button() != Qt::LeftButton || !isDragArea(skinPos)) {
        return;
    }

    if (!CanPositionWindows()) {
        if (QWindow* window = windowHandle()) {
            window->startSystemMove();
        }
        return;
    }
    dragging = true;
    pressGlobalPosition = event->globalPosition().toPoint();
    dragGroup.clear();
    dragGroup.append({this, pos()});
    if (dragsDocked) {
        for (SkinnedWindow* window : dockedWindows()) {
            dragGroup.append({window, window->pos()});
        }
    }
    groupStartBounds = QRect();
    for (const auto& [window, start] : dragGroup) {
        groupStartBounds |= QRect(start, window->size());
    }
}

void SkinnedWindow::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging) {
        skinMouseMove(toSkin(event->position()));
        return;
    }
    const QPoint delta = event->globalPosition().toPoint() - pressGlobalPosition;
    QList<QRect> others;
    for (SkinnedWindow* window : WindowRegistry()) {
        if (!window->isVisible()) {
            continue;
        }
        const bool inGroup =
            std::any_of(dragGroup.cbegin(), dragGroup.cend(), [window](const auto& member) {
                return member.first == window;
            });
        if (!inGroup) {
            others << window->frameGeometry();
        }
    }
    const QPoint target =
        Ui::ResolveDragPosition(groupStartBounds.translated(delta), others, ScreenRects());
    const QPoint applied = target - groupStartBounds.topLeft();
    for (const auto& [window, start] : dragGroup) {
        if (window && window->pos() != start + applied) {
            window->move(start + applied);
        }
    }
}

void SkinnedWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging && event->button() == Qt::LeftButton) {
        dragging = false;
        dragGroup.clear();
        Q_EMIT moveFinished();
        return;
    }
    skinMouseRelease(toSkin(event->position()), event->button());
}

void SkinnedWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!skinMouseDoubleClick(toSkin(event->position()), event->button())) {
        mousePressEvent(event);
    }
}

void SkinnedWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::ActivationChange) {
        update();
    } else if (event->type() == QEvent::LanguageChange) {
        retranslate();
        update();  // the texts are drawn, so a repaint takes the new language
    }
    QWidget::changeEvent(event);
}

}  // namespace Ui
