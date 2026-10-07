#include "ui/milkdrop_window.h"

#include "audio/audio_engine.h"
#include "skins/skin.h"
#include "vis/milkdrop_view.h"

#include <QActionGroup>
#include <QByteArray>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFont>
#include <QGuiApplication>
#include <QMenu>
#include <QPainter>
#include <QPoint>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QScreen>
#include <QUrl>
#include <QWidget>

#include <algorithm>

namespace Ui {

namespace {
constexpr int kHistoryLimit = 100;
constexpr int kFpsPlaying = 60;
constexpr int kFpsIdle = 20;
}  // namespace

MilkdropWindow::MilkdropWindow(
    Audio::AudioEngine* engine,
    const QString& builtInPresetDirectory,
    const QString& userPresetDirectory,
    const Skins::Skin* skin,
    QWidget* parent
)
    : GenWindow(skin, QStringLiteral("MILKDROP"), parent)
    , audioEngine(engine)
    , builtInDirectory(builtInPresetDirectory)
    , userDirectory(userPresetDirectory) {
    setWindowTitle(QStringLiteral("QiYaa: Milkdrop"));
    presetList.load(builtInDirectory, userDirectory);
}

void MilkdropWindow::ensureView() {
    if (viewTried) {
        return;
    }
    viewTried = true;
    openGlProblem = Vis::MilkdropView::OpenGlProblem();
    if (!openGlProblem.isEmpty()) {
        qWarning("Milkdrop unavailable: %s", qPrintable(openGlProblem));
        update();
        return;
    }
    milkdropView = new Vis::MilkdropView(audioEngine);
    container = QWidget::createWindowContainer(milkdropView, this);
    container->setFocusPolicy(Qt::ClickFocus);
    wireView(milkdropView);
    placeView();
    container->show();
}

QString MilkdropWindow::failure() const {
    if (!openGlProblem.isEmpty()) {
        return openGlProblem;
    }
    return milkdropView ? milkdropView->failure() : QString();
}

MilkdropWindow::~MilkdropWindow() = default;

QString MilkdropWindow::currentPreset() const {
    return selectedIndex >= 0 && selectedIndex < presetList.size()
        ? presetList.at(selectedIndex).name
        : QString();
}

void MilkdropWindow::wireView(Vis::MilkdropView* view) {
    view->setTextureSearchPaths({userDirectory, userDirectory + QStringLiteral("/textures")});
    view->setPresetDuration(switchIntervalSeconds);
    view->setLocked(lockEnabled);
    connect(view, &Vis::MilkdropView::ready, this, [this, view] {
        if (selectedIndex < 0 && !presetList.isEmpty()) {
            selectedIndex = followingPreset();
        }
        if (selectedIndex >= 0) {
            view->loadPreset(presetList.data(selectedIndex), Vis::PresetTransition::Cut);
        }
        if (selectedIndex >= 0) {
            Q_EMIT presetChanged(currentPreset(), PresetOrigin::Automatic);
        }
    });
    connect(view, &Vis::MilkdropView::failed, this, [this, view](const QString& reason) {
        qWarning("Milkdrop unavailable: %s", qPrintable(reason));
        if (view == milkdropView) {
            container->hide();
            milkdropView->setRendering(false);
        } else {
            setFullScreenMode(false);
        }
        update();
    });
    connect(view, &Vis::MilkdropView::switchRequested, this, &MilkdropWindow::onSwitchRequested);
    connect(view, &Vis::MilkdropView::presetFailed, this, &MilkdropWindow::onPresetFailed);
    connect(view, &Vis::MilkdropView::staysBlack, this, &MilkdropWindow::onStaysBlack);
    connect(view, &Vis::MilkdropView::drawsPicture, this, [this] { blackInARow = 0; });
    view->setBlackWatch(musicPlaying && blackInARow <= 5);
    connect(view, &Vis::MilkdropView::doubleClicked, this, [this] {
        setFullScreenMode(!isFullScreenMode());
    });
    connect(view, &Vis::MilkdropView::contextMenuRequested, this, &MilkdropWindow::showMenu);
    connect(view, &Vis::MilkdropView::keyPressed, this, &MilkdropWindow::handleKey);
}

void MilkdropWindow::selectPreset(
    int index,
    Vis::PresetTransition transition,
    PresetOrigin origin
) {
    if (index < 0 || index >= presetList.size()) {
        return;
    }
    const QByteArray presetData = presetList.data(index);
    if (presetData.isEmpty()) {
        return;
    }
    if (selectedIndex >= 0 && selectedIndex != index) {
        history.append(selectedIndex);
        if (history.size() > kHistoryLimit) {
            history.removeFirst();
        }
    }
    selectedIndex = index;
    if (Vis::MilkdropView* view = fullView ? fullView.get() : milkdropView) {
        view->loadPreset(presetData, transition);
    }
    Q_EMIT presetChanged(currentPreset(), origin);
    Q_EMIT settingsChanged();
}

void MilkdropWindow::selectPreset(const QString& name) {
    const int index = presetList.indexOf(name);
    if (index < 0) {
        return;
    }
    if (!milkdropView || !milkdropView->isReady()) {
        selectedIndex = index;
        return;
    }
    selectPreset(index, Vis::PresetTransition::Cut);
}

void MilkdropWindow::nextPreset() {
    failuresInARow = 0;
    selectPreset(followingPreset());
}

void MilkdropWindow::previousPreset() {
    failuresInARow = 0;
    int index = -1;
    if (shuffleEnabled && !history.isEmpty()) {
        index = history.takeLast();
        const int keptIndex = selectedIndex;
        selectedIndex = -1;  // going back: don't record where we came from
        selectPreset(index);
        if (selectedIndex != index) {
            selectedIndex = keptIndex;
        }
        return;
    }
    int previous = presetList.previous(selectedIndex);
    for (int step = 0; step < presetList.size() && isBlack(previous); ++step) {
        previous = presetList.previous(previous);
    }
    selectPreset(previous);
}

void MilkdropWindow::reloadPresets() {
    const QString current = currentPreset();
    presetList.load(builtInDirectory, userDirectory);
    history.clear();
    selectedIndex = presetList.indexOf(current);
}

void MilkdropWindow::setShuffle(bool on) {
    if (on == shuffleEnabled) {
        return;
    }
    shuffleEnabled = on;
    Q_EMIT settingsChanged();
}

void MilkdropWindow::setLocked(bool on) {
    if (on == lockEnabled) {
        return;
    }
    lockEnabled = on;
    if (milkdropView) {
        milkdropView->setLocked(on);
    }
    if (fullView) {
        fullView->setLocked(on);
    }
    Q_EMIT settingsChanged();
}

void MilkdropWindow::setPresetSeconds(int seconds) {
    seconds = std::clamp(seconds, 5, 3600);
    if (seconds == switchIntervalSeconds) {
        return;
    }
    switchIntervalSeconds = seconds;
    if (milkdropView) {
        milkdropView->setPresetDuration(seconds);
    }
    if (fullView) {
        fullView->setPresetDuration(seconds);
    }
    Q_EMIT settingsChanged();
}

void MilkdropWindow::setPlaying(bool playing) {
    if (playing == musicPlaying) {
        return;
    }
    musicPlaying = playing;
    if (milkdropView) {
        milkdropView->setBlackWatch(playing && blackInARow <= 5);
    }
    if (fullView) {
        fullView->setBlackWatch(playing && blackInARow <= 5);
    }
    updateRendering();
}

int MilkdropWindow::fps() const {
    return musicPlaying ? kFpsPlaying : kFpsIdle;
}

void MilkdropWindow::updateRendering() {
    if (!milkdropView) {
        return;
    }
    if (fullView) {
        milkdropView->setRendering(false);
        fullView->setRendering(true, fps());
    } else {
        milkdropView->setRendering(isVisible() && milkdropView->failure().isEmpty(), fps());
    }
}

void MilkdropWindow::setFullScreenMode(bool on) {
    if (on == isFullScreenMode()) {
        return;
    }
    if (on) {
        if (!milkdropView || !milkdropView->failure().isEmpty()) {
            return;
        }
        fullView = std::make_unique<Vis::MilkdropView>(audioEngine);
        wireView(fullView.get());
        fullView->setTitle(QStringLiteral("QiYaa: Milkdrop"));
        fullView->setCursor(Qt::BlankCursor);
        if (QScreen* targetScreen = screen()) {
            fullView->setScreen(targetScreen);
            fullView->setGeometry(targetScreen->geometry());
        }
        fullView->showFullScreen();
        fullView->requestActivate();
    } else {
        // Possibly called from one of its own event handlers: delete it later.
        Vis::MilkdropView* view = fullView.release();
        view->setRendering(false);
        view->hide();
        view->deleteLater();
        if (selectedIndex >= 0 && milkdropView) {
            milkdropView->loadPreset(presetList.data(selectedIndex), Vis::PresetTransition::Cut);
        }
    }
    updateRendering();
}

int MilkdropWindow::followingPreset() const {
    if (presetList.isEmpty()) {
        return -1;
    }
    if (shuffleEnabled) {
        QList<int> candidates;
        for (int i = 0; i < presetList.size(); ++i) {
            if (i != selectedIndex && !isBlack(i)) {
                candidates.append(i);
            }
        }
        if (!candidates.isEmpty()) {
            return candidates.at(
                static_cast<int>(QRandomGenerator::global()->bounded(candidates.size()))
            );
        }
    } else {
        int index = selectedIndex;
        const int otherCount = selectedIndex >= 0 ? presetList.size() - 1 : presetList.size();
        for (int step = 0; step < otherCount; ++step) {
            index = presetList.next(index);
            if (!isBlack(index)) {
                return index;
            }
        }
    }
    if (selectedIndex >= 0 && !isBlack(selectedIndex)) {
        return selectedIndex;
    }
    return shuffleEnabled ? presetList.random(selectedIndex) : presetList.next(selectedIndex);
}

bool MilkdropWindow::isBlack(int index) const {
    return index >= 0 && index < presetList.size()
        && blackPresetNames.contains(presetList.at(index).name);
}

QStringList MilkdropWindow::blackPresets() const {
    QStringList names(blackPresetNames.cbegin(), blackPresetNames.cend());
    names.sort();
    return names;
}

void MilkdropWindow::setBlackPresets(const QStringList& names) {
    blackPresetNames = QSet<QString>(names.cbegin(), names.cend());
    blackInARow = 0;
}

void MilkdropWindow::onStaysBlack() {
    const QString name = currentPreset();
    if (name.isEmpty()) {
        return;
    }
    const Vis::MilkdropView* view = fullView ? fullView.get() : milkdropView;
    qWarning(
        "Milkdrop: \"%s\" shows only black here (%s), skipping it", qPrintable(name),
        view ? qPrintable(view->glInfo()) : "?"
    );
    if (++blackInARow > 5) {
        qWarning("Milkdrop: many presets in a row stay black; not skipping any more");
        if (milkdropView) {
            milkdropView->setBlackWatch(false);
        }
        if (fullView) {
            fullView->setBlackWatch(false);
        }
        return;
    }
    blackPresetNames.insert(name);
    Q_EMIT settingsChanged();
    failuresInARow = 0;
    selectPreset(followingPreset(), Vis::PresetTransition::Cut, PresetOrigin::Automatic);
}

void MilkdropWindow::onSwitchRequested(Vis::PresetTransition transition) {
    if (lockEnabled || presetList.isEmpty()) {
        return;
    }
    failuresInARow = 0;
    blackInARow = 0;
    selectPreset(followingPreset(), transition, PresetOrigin::Automatic);
}

void MilkdropWindow::onPresetFailed(const QString& message) {
    qWarning("Milkdrop preset \"%s\" failed: %s", qPrintable(currentPreset()), qPrintable(message));
    if (++failuresInARow >= std::min(10, presetList.size())) {
        return;
    }
    selectPreset(followingPreset(), Vis::PresetTransition::Cut, PresetOrigin::Automatic);
}

void MilkdropWindow::handleKey(int key, Qt::KeyboardModifiers modifiers) {
    switch (key) {
        case Qt::Key_Space:
        case Qt::Key_N: nextPreset(); break;
        case Qt::Key_Backspace:
        case Qt::Key_P: previousPreset(); break;
        case Qt::Key_H:
            failuresInARow = 0;
            selectPreset(followingPreset(), Vis::PresetTransition::Cut);
            break;
        case Qt::Key_R: setShuffle(!shuffleEnabled); break;
        case Qt::Key_L:
        case Qt::Key_ScrollLock: setLocked(!lockEnabled); break;
        case Qt::Key_F: setFullScreenMode(!isFullScreenMode()); break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (modifiers & Qt::AltModifier) {
                setFullScreenMode(!isFullScreenMode());
            }
            break;
        case Qt::Key_Escape: setFullScreenMode(false); break;
        case Qt::Key_K:
            if ((modifiers & Qt::ControlModifier) && (modifiers & Qt::ShiftModifier)) {
                setFullScreenMode(false);
                Q_EMIT closeRequested();
            }
            break;
        case Qt::Key_Z:
        case Qt::Key_X:
        case Qt::Key_C:
        case Qt::Key_V:
        case Qt::Key_B:
        case Qt::Key_Left:
        case Qt::Key_Right: Q_EMIT transportKey(key); break;
        default: break;
    }
}

void MilkdropWindow::showMenu(const QPoint& globalPos) {
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(tr("Next preset\tSpace"), this, &MilkdropWindow::nextPreset);
    menu->addAction(tr("Previous preset\tBackspace"), this, &MilkdropWindow::previousPreset);
    if (!presetList.isEmpty()) {
        QMenu* presetsMenu = menu->addMenu(tr("Presets"));
        for (int i = 0; i < presetList.size(); ++i) {
            const QString label = isBlack(i) ? tr("%1  (black here)").arg(presetList.at(i).name)
                                             : presetList.at(i).name;
            QAction* action = presetsMenu->addAction(label, this, [this, i] {
                failuresInARow = 0;
                selectPreset(i);
            });
            action->setCheckable(true);
            action->setChecked(i == selectedIndex);
        }
    }
    menu->addSeparator();
    QAction* shuffle =
        menu->addAction(tr("Random order\tR"), this, [this](bool on) { setShuffle(on); });
    shuffle->setCheckable(true);
    shuffle->setChecked(shuffleEnabled);
    QAction* lock =
        menu->addAction(tr("Do not switch by itself\tL"), this, [this](bool on) { setLocked(on); });
    lock->setCheckable(true);
    lock->setChecked(lockEnabled);
    QMenu* intervalMenu = menu->addMenu(tr("Change the preset every"));
    auto* group = new QActionGroup(intervalMenu);
    for (int seconds : {15, 30, 60, 120, 300}) {
        const QString label =
            seconds < 60 ? tr("%1 s").arg(seconds) : tr("%1 min").arg(seconds / 60);
        QAction* action =
            intervalMenu->addAction(label, this, [this, seconds] { setPresetSeconds(seconds); });
        action->setCheckable(true);
        action->setChecked(seconds == switchIntervalSeconds);
        group->addAction(action);
    }
    menu->addSeparator();
    QAction* fullScreen =
        menu->addAction(tr("Full screen\tF"), this, [this](bool on) { setFullScreenMode(on); });
    fullScreen->setCheckable(true);
    fullScreen->setChecked(isFullScreenMode());
    fullScreen->setEnabled(failure().isEmpty());
    menu->addSeparator();
    menu->addAction(tr("Open the folder of your presets"), this, [this] {
        QDir().mkpath(userDirectory);
        QDesktopServices::openUrl(QUrl::fromLocalFile(userDirectory));
    });
    menu->addAction(tr("Reload the presets"), this, &MilkdropWindow::reloadPresets);
    menu->addAction(
            tr("Copy the preset's name"), this,
            [this] { QGuiApplication::clipboard()->setText(currentPreset()); }
    )->setEnabled(selectedIndex >= 0);
    if (!blackPresetNames.isEmpty()) {
        menu->addAction(
            tr("Bring back the skipped black presets (%1)").arg(blackPresetNames.size()), this,
            [this] {
                setBlackPresets({});
                if (milkdropView) {
                    milkdropView->setBlackWatch(musicPlaying);
                }
                if (fullView) {
                    fullView->setBlackWatch(musicPlaying);
                }
                Q_EMIT settingsChanged();
            }
        );
    }
    menu->popup(globalPos);
}

void MilkdropWindow::paintContent(QPainter& painter, const QRect& area) {
    painter.fillRect(area, Qt::black);
    const QString failureText = this->failure();
    if (failureText.isEmpty()) {
        return;
    }
    QFont messageFont = painter.font();
    messageFont.setPixelSize(9);
    painter.setFont(messageFont);
    painter.setPen(QColor(0, 200, 0));
    painter.drawText(
        area.adjusted(4, 4, -4, -4), Qt::AlignCenter | Qt::TextWordWrap,
        tr("Milkdrop is not available: %1").arg(failureText)
    );
}

bool MilkdropWindow::contentMousePress(QPoint, Qt::MouseButton button) {
    if (button == Qt::RightButton) {
        showMenu(QCursor::pos());
        return true;
    }
    return false;
}

void MilkdropWindow::placeView() {
    if (!container) {
        return;
    }
    const QRect rect = contentRect();
    const double windowScale = scale();
    const int left = qRound(rect.left() * windowScale), top = qRound(rect.top() * windowScale);
    const int right = qRound((rect.right() + 1) * windowScale),
              bottom = qRound((rect.bottom() + 1) * windowScale);
    container->setGeometry(left, top, right - left, bottom - top);
}

void MilkdropWindow::resizeEvent(QResizeEvent* event) {
    GenWindow::resizeEvent(event);
    placeView();
}

void MilkdropWindow::showEvent(QShowEvent* event) {
    GenWindow::showEvent(event);
    ensureView();
    updateRendering();
}

void MilkdropWindow::hideEvent(QHideEvent* event) {
    GenWindow::hideEvent(event);
    setFullScreenMode(false);
    updateRendering();
}

}  // namespace Ui
