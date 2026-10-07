#include "ui/equalizer_window.h"

#include "audio/eq_presets.h"
#include "audio/error.h"
#include "skins/skin.h"
#include "skins/sprites.h"
#include "ui/input_dialogs.h"

#include <QCloseEvent>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QList>
#include <QMenu>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace Ui {

using Audio::EqSettings;
using Audio::kEqBandHz;
using Audio::kEqBands;
using Audio::kEqMaxDb;
using TSheet = Skins::Skin::Sheet;
using Skins::EqualizerSprites;

namespace {

// A preset library of a thousand presets is 268 KB.
constexpr qint64 kMaxEqfBytes = 1024 * 1024;

using TControl = EqualizerControl::Kind;

constexpr QPoint kShadeButton{254, 3};
using Skins::EqualizerShadeSprites;

bool Contains(const QRect& rect, QPoint point) {
    return point.x() >= rect.x() && point.y() >= rect.y() && point.x() < rect.x() + rect.width()
        && point.y() < rect.y() + rect.height();
}

QRect SliderRect(const EqualizerControl& control) {
    const int x = control.kind == TControl::Preamp
        ? Skins::EqualizerSprites::kPreampPosition.x()
        : Skins::EqualizerSprites::kBandsX + control.band * Skins::EqualizerSprites::kBandStep;
    return {
        x, Skins::EqualizerSprites::kSlidersY, Skins::EqualizerSprites::kSliderSize.width(),
        Skins::EqualizerSprites::kSliderSize.height()
    };
}

QString BandName(int band) {
    const double hz = Audio::kEqBandHz[band];
    return hz >= 1000 ? QStringLiteral("%1KHZ").arg(hz / 1000) : QStringLiteral("%1HZ").arg(hz);
}

// Port of webamp's spline.js (itself adapted from morganherlocker/cubic-spline, MIT).
QList<double> NaturalSpline(const QList<double>& xs, const QList<double>& ys) {
    const int n = static_cast<int>(xs.size()) - 1;
    QList<double> a(n + 1), b(n + 1), c(n + 1), d(n + 1), k(n + 1);
    for (int i = 0; i <= n; ++i) {
        if (i == 0) {
            const double h = xs[1] - xs[0];
            b[i] = 2 / h;
            c[i] = 1 / h;
            d[i] = 3 * (ys[1] - ys[0]) / (h * h);
        } else if (i == n) {
            const double h = xs[n] - xs[n - 1];
            a[i] = 1 / h;
            b[i] = 2 / h;
            d[i] = 3 * (ys[n] - ys[n - 1]) / (h * h);
        } else {
            const double h0 = xs[i] - xs[i - 1], h1 = xs[i + 1] - xs[i];
            a[i] = 1 / h0;
            b[i] = 2 * (1 / h0 + 1 / h1);
            c[i] = 1 / h1;
            d[i] = 3 * ((ys[i] - ys[i - 1]) / (h0 * h0) + (ys[i + 1] - ys[i]) / (h1 * h1));
        }
    }
    for (int i = 1; i <= n; ++i) {
        const double m = a[i] / b[i - 1];
        b[i] -= m * c[i - 1];
        d[i] -= m * d[i - 1];
    }
    k[n] = d[n] / b[n];
    for (int i = n - 1; i >= 0; --i) {
        k[i] = (d[i] - c[i] * k[i + 1]) / b[i];
    }

    QList<double> curve;
    int i = 1;
    for (int x = 0; x <= static_cast<int>(xs[n]); ++x) {
        while (i < n && xs[i] < x) {
            ++i;
        }
        const double h = xs[i] - xs[i - 1];
        const double t = (x - xs[i - 1]) / h;
        const double aa = k[i - 1] * h - (ys[i] - ys[i - 1]);
        const double bb = -k[i] * h + (ys[i] - ys[i - 1]);
        curve << (1 - t) * ys[i - 1] + t * ys[i] + t * (1 - t) * (aa * (1 - t) + bb * t);
    }
    return curve;
}

constexpr int kGraphHeight = 19;

double DbToGraphY(double db) {
    return (1.0 - (db + Audio::kEqMaxDb) / (2 * Audio::kEqMaxDb)) * (kGraphHeight - 1);
}

}  // namespace

EqualizerWindow::EqualizerWindow(const Skins::Skin* skin, QWidget* parent)
    : SkinnedWindow(skin, Skins::EqualizerSprites::kSize, parent) {
    retranslate();
}

void EqualizerWindow::setAutoOn(bool on) {
    autoEnabled = on;
    update();
}

void EqualizerWindow::setSettings(const Audio::EqSettings& settings) {
    equalizerSettings = settings;
    update();
}

QList<double> EqualizerWindow::GraphCurve(const Audio::EqSettings& settings) {
    QList<double> xs, ys;
    for (int i = 0; i < Audio::kEqBands; ++i) {
        xs << i * 12.0;
        ys << DbToGraphY(settings.bandsDb[i]);
    }
    return NaturalSpline(xs, ys);
}

void EqualizerWindow::drawSlider(QPainter& painter, QPoint at, double db, bool active) const {
    const int frame =
        static_cast<int>(std::lround((db + Audio::kEqMaxDb) / (2 * Audio::kEqMaxDb) * 27));
    const QRect source(
        Skins::EqualizerSprites::kSliderFrames.x() + (frame % 14) * 15,
        Skins::EqualizerSprites::kSliderFrames.y() + (frame / 14) * 65,
        Skins::EqualizerSprites::kSliderSize.width(), Skins::EqualizerSprites::kSliderSize.height()
    );
    skin().draw(painter, TSheet::EqMain, source, at);
    const int thumbY = static_cast<int>(std::lround(
        (1.0 - (db + Audio::kEqMaxDb) / (2 * Audio::kEqMaxDb))
        * Skins::EqualizerSprites::kSliderTravel
    ));
    skin().draw(
        painter, TSheet::EqMain,
        active ? Skins::EqualizerSprites::kThumbSelected : Skins::EqualizerSprites::kThumb,
        at + QPoint(1, thumbY)
    );
}

void EqualizerWindow::drawGraph(QPainter& painter) const {
    const QPoint origin = Skins::EqualizerSprites::kGraphPosition;
    skin().draw(painter, TSheet::EqMain, Skins::EqualizerSprites::kGraphBackground, origin);
    skin().draw(
        painter, TSheet::EqMain, Skins::EqualizerSprites::kPreampLine,
        origin + QPoint(0, static_cast<int>(std::lround(DbToGraphY(equalizerSettings.preampDb))))
    );

    const QImage& sheet = skin().sheet(TSheet::EqMain);
    const QList<double> ys = GraphCurve(equalizerSettings);
    int lastY = static_cast<int>(std::lround(ys.first()));
    for (int x = 0; x < ys.size(); ++x) {
        const int y = std::clamp(static_cast<int>(std::lround(ys[x])), 0, kGraphHeight - 1);
        const int top = std::min(y, lastY), bottom = std::max(y, lastY);
        for (int row = top; row <= bottom; ++row) {
            const QColor color = sheet.isNull()
                ? QColor(Qt::green)
                : QColor::fromRgb(sheet.pixel(
                      Skins::EqualizerSprites::kGraphLineColors.x(),
                      Skins::EqualizerSprites::kGraphLineColors.y() + row
                  ));
            painter.fillRect(origin.x() + 2 + x, origin.y() + row, 1, 1, color);
        }
        lastY = y;
    }
}

void EqualizerWindow::setShaded(bool shaded) {
    if (shaded == isShaded()) {
        return;
    }
    applyShade(shaded, shaded ? QSize(275, 14) : Skins::EqualizerSprites::kSize);
    update();
}

void EqualizerWindow::setMixer(int volume, int balance) {
    volumePercent = volume;
    balancePercent = balance;
    if (isShaded()) {
        update();
    }
}

void EqualizerWindow::paintShaded(QPainter& painter) {
    const Skins::Skin& activeSkin = skin();
    activeSkin.draw(
        painter, TSheet::EqEx,
        isActiveWindow() ? Skins::EqualizerShadeSprites::kShadeBackgroundSelected
                         : Skins::EqualizerShadeSprites::kShadeBackground,
        {0, 0}
    );
    const int volumeThumb = std::clamp(volumePercent * 3 / 101, 0, 2);
    const int volumeX =
        Skins::EqualizerShadeSprites::kVolume.x()
        + static_cast<int>(
            std::lround(volumePercent / 100.0 * (Skins::EqualizerShadeSprites::kVolume.width() - 3))
        );
    activeSkin.draw(
        painter, TSheet::EqEx, Skins::EqualizerShadeSprites::kVolumeThumb[volumeThumb],
        {volumeX, Skins::EqualizerShadeSprites::kVolume.y()}
    );
    const int balanceThumb = std::clamp((balancePercent + 100) * 3 / 201, 0, 2);
    const int balanceX = Skins::EqualizerShadeSprites::kBalance.x()
        + static_cast<int>(std::lround(
            (balancePercent + 100) / 200.0 * (Skins::EqualizerShadeSprites::kBalance.width() - 3)
        ));
    activeSkin.draw(
        painter, TSheet::EqEx, Skins::EqualizerShadeSprites::kBalanceThumb[balanceThumb],
        {balanceX, Skins::EqualizerShadeSprites::kBalance.y()}
    );
    if (pressedControl.kind == TControl::Shade && pressedInside) {
        activeSkin.draw(
            painter, TSheet::EqEx, Skins::EqualizerShadeSprites::kShadeButtonShadedDown,
            kShadeButton
        );
    }
    if (pressedControl.kind == TControl::Close && pressedInside) {
        activeSkin.draw(
            painter, TSheet::EqEx, Skins::EqualizerShadeSprites::kCloseButtonDown,
            Skins::EqualizerSprites::kClose
        );
    }
}

void EqualizerWindow::paintSkin(QPainter& painter) {
    if (isShaded()) {
        return paintShaded(painter);
    }
    const Skins::Skin& activeSkin = skin();
    activeSkin.draw(painter, TSheet::EqMain, Skins::EqualizerSprites::kBackground, {0, 0});
    activeSkin.draw(
        painter, TSheet::EqMain,
        isActiveWindow() ? Skins::EqualizerSprites::kTitleBarSelected
                         : Skins::EqualizerSprites::kTitleBar,
        {0, 0}
    );
    if (pressedControl.kind == TControl::Close && pressedInside) {
        activeSkin.draw(
            painter, TSheet::EqMain, Skins::EqualizerSprites::kCloseButtonDown,
            Skins::EqualizerSprites::kClose
        );
    }
    if (pressedControl.kind == TControl::Shade && pressedInside) {
        activeSkin.draw(
            painter, TSheet::EqEx, Skins::EqualizerShadeSprites::kShadeButtonDown, kShadeButton
        );
    }

    auto toggle = [&](TControl kind, const Skins::ToggleSprite& sprite, bool on, QPoint at) {
        const bool down = pressedControl.kind == kind && pressedInside;
        activeSkin.draw(
            painter, TSheet::EqMain,
            on ? (down ? sprite.onPressed : sprite.on) : (down ? sprite.offPressed : sprite.off), at
        );
    };
    toggle(
        TControl::On, Skins::EqualizerSprites::kOn, equalizerSettings.enabled,
        Skins::EqualizerSprites::kOnPosition
    );
    toggle(
        TControl::Auto, Skins::EqualizerSprites::kAuto, autoEnabled,
        Skins::EqualizerSprites::kAutoPosition
    );
    activeSkin.draw(
        painter, TSheet::EqMain,
        pressedControl.kind == TControl::Presets && pressedInside
            ? Skins::EqualizerSprites::kPresetsButtonSelected
            : Skins::EqualizerSprites::kPresetsButton,
        Skins::EqualizerSprites::kPresetsPosition
    );

    drawGraph(painter);
    drawSlider(
        painter, SliderRect({TControl::Preamp}).topLeft(), equalizerSettings.preampDb,
        pressedControl.kind == TControl::Preamp
    );
    for (int band = 0; band < Audio::kEqBands; ++band) {
        const EqualizerControl slider{TControl::Band, band};
        drawSlider(
            painter, SliderRect(slider).topLeft(), equalizerSettings.bandsDb[band],
            pressedControl == slider
        );
    }
}

EqualizerControl EqualizerWindow::hitTest(QPoint point) const {
    if (Contains({Skins::EqualizerSprites::kClose, QSize(9, 9)}, point)) {
        return {TControl::Close};
    }
    if (Contains({kShadeButton, QSize(9, 9)}, point)) {
        return {TControl::Shade};
    }
    if (isShaded()) {
        if (Contains(Skins::EqualizerShadeSprites::kVolume, point)) {
            return {TControl::ShadeVolume};
        }
        if (Contains(Skins::EqualizerShadeSprites::kBalance, point)) {
            return {TControl::ShadeBalance};
        }
        return {};
    }
    if (Contains({Skins::EqualizerSprites::kOnPosition, QSize(26, 12)}, point)) {
        return {TControl::On};
    }
    if (Contains({Skins::EqualizerSprites::kAutoPosition, QSize(32, 12)}, point)) {
        return {TControl::Auto};
    }
    if (Contains({Skins::EqualizerSprites::kPresetsPosition, QSize(44, 12)}, point)) {
        return {TControl::Presets};
    }
    if (Contains(SliderRect({TControl::Preamp}), point)) {
        return {TControl::Preamp};
    }
    for (int band = 0; band < Audio::kEqBands; ++band) {
        const EqualizerControl slider{TControl::Band, band};
        if (Contains(SliderRect(slider), point)) {
            return slider;
        }
    }
    return {};
}

QString EqualizerWindow::regionSection() const {
    return isShaded() ? QStringLiteral("equalizerws") : QStringLiteral("equalizer");
}

bool EqualizerWindow::isDragArea(QPoint skinPos) const {
    return hitTest(skinPos).kind == TControl::None;
}

double* EqualizerWindow::valueFor(const EqualizerControl& control) {
    if (control.kind == TControl::Preamp) {
        return &equalizerSettings.preampDb;
    }
    if (control.kind == TControl::Band && control.band >= 0 && control.band < Audio::kEqBands) {
        return &equalizerSettings.bandsDb[control.band];
    }
    return nullptr;
}

void EqualizerWindow::changed(const EqualizerControl& control) {
    Q_EMIT settingsChanged(equalizerSettings);
    if (const double* value = valueFor(control)) {
        const QString name =
            control.kind == TControl::Preamp ? QStringLiteral("PREAMP") : BandName(control.band);
        Q_EMIT statusText(QStringLiteral("EQ: %1 %2%3 DB")
                              .arg(name, *value >= 0 ? QStringLiteral("+") : QString())
                              .arg(*value, 0, 'f', 1));
    }
    update();
}

void EqualizerWindow::setFromMouse(const EqualizerControl& control, QPoint point) {
    if (control.kind == TControl::ShadeVolume || control.kind == TControl::ShadeBalance) {
        const QRect rect = control.kind == TControl::ShadeVolume
            ? Skins::EqualizerShadeSprites::kVolume
            : Skins::EqualizerShadeSprites::kBalance;
        const double fraction =
            std::clamp((point.x() - rect.x() - 1.5) / (rect.width() - 3), 0.0, 1.0);
        if (control.kind == TControl::ShadeVolume) {
            Q_EMIT volumeRequested(static_cast<int>(std::lround(fraction * 100)));
        } else {
            Q_EMIT balanceRequested(static_cast<int>(std::lround(fraction * 200 - 100)));
        }
        return;
    }
    double* value = valueFor(control);
    if (!value) {
        return;
    }
    const QRect rect = SliderRect(control);
    const double top = std::clamp(
        static_cast<double>(point.y() - rect.y()) - 5.5, 0.0,
        static_cast<double>(Skins::EqualizerSprites::kSliderTravel)
    );
    double db =
        Audio::kEqMaxDb - top / Skins::EqualizerSprites::kSliderTravel * 2 * Audio::kEqMaxDb;
    db = std::round(db * 10) / 10;
    if (std::abs(db) < 0.6) {
        db = 0;
    }
    if (db == *value) {
        return;
    }
    *value = db;
    changed(control);
}

bool EqualizerWindow::skinMousePress(QPoint pos, Qt::MouseButton button) {
    if (button != Qt::LeftButton) {
        return false;
    }
    const EqualizerControl control = hitTest(pos);
    if (control.kind == TControl::None) {
        return false;
    }
    pressedControl = control;
    pressedInside = true;
    setFromMouse(control, pos);
    update();
    return true;
}

void EqualizerWindow::skinMouseMove(QPoint pos) {
    if (pressedControl.kind == TControl::None) {
        return;
    }
    if (valueFor(pressedControl) || pressedControl.kind == TControl::ShadeVolume
        || pressedControl.kind == TControl::ShadeBalance) {
        setFromMouse(pressedControl, pos);
    } else {
        pressedInside = hitTest(pos) == pressedControl;
    }
    update();
}

void EqualizerWindow::skinMouseRelease(QPoint pos, Qt::MouseButton button) {
    if (button != Qt::LeftButton || pressedControl.kind == TControl::None) {
        return;
    }
    const EqualizerControl released = pressedControl;
    const bool inside = hitTest(pos) == released;
    pressedControl = {};
    update();
    if (!inside) {
        return;
    }
    switch (released.kind) {
        case TControl::Close: Q_EMIT closeRequested(); break;
        case TControl::Shade: setShaded(!isShaded()); break;
        case TControl::On:
            equalizerSettings.enabled = !equalizerSettings.enabled;
            Q_EMIT settingsChanged(equalizerSettings);
            Q_EMIT statusText(
                equalizerSettings.enabled ? QStringLiteral("EQ: ON") : QStringLiteral("EQ: OFF")
            );
            break;
        case TControl::Auto: autoEnabled = !autoEnabled; break;
        case TControl::Presets: showPresets(); break;
        default: break;
    }
}

bool EqualizerWindow::skinMouseDoubleClick(QPoint pos, Qt::MouseButton button) {
    const EqualizerControl control = hitTest(pos);
    if (button == Qt::LeftButton && pos.y() < 14 && control.kind == TControl::None) {
        setShaded(!isShaded());
        return true;
    }
    double* value = valueFor(control);
    if (button != Qt::LeftButton || !value) {
        return false;
    }
    *value = 0;
    changed(control);
    return true;
}

void EqualizerWindow::wheelEvent(QWheelEvent* event) {
    const EqualizerControl control = hitTest(toSkin(event->position()));
    double* value = valueFor(control);
    const int steps = wheelSteps(event);
    if (!value || steps == 0) {
        return;
    }
    *value =
        std::clamp(std::round((*value + steps * 0.5) * 10) / 10, -Audio::kEqMaxDb, Audio::kEqMaxDb);
    changed(control);
}

void EqualizerWindow::applyPreset(const Audio::EqPreset& preset) {
    const bool enabled = equalizerSettings.enabled;
    equalizerSettings = preset.settings;
    equalizerSettings.enabled = enabled;
    Q_EMIT settingsChanged(equalizerSettings);
    Q_EMIT statusText(QStringLiteral("EQ: ") + preset.name.toUpper());
    update();
}

void EqualizerWindow::loadEqf() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Equalizer preset"), QDir::homePath(), tr("Winamp presets (*.eqf *.EQF *.q1)")
    );
    if (path.isEmpty()) {
        return;
    }
    const QString fileName = QFileInfo(path).fileName();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        Q_EMIT statusText(QStringLiteral("EQ: %1: %2").arg(fileName, file.errorString()));
        return;
    }
    if (file.size() > kMaxEqfBytes) {
        Q_EMIT statusText(tr("EQ: %1 is %2 KB, and presets are %3 KB at most")
                              .arg(fileName)
                              .arg(file.size() / 1024)
                              .arg(kMaxEqfBytes / 1024));
        return;
    }
    QList<Audio::EqPreset> presets;
    try {
        presets = Audio::ParseEqf(file.readAll());
    } catch (const Audio::Error& error) {
        Q_EMIT statusText(
            QStringLiteral("EQ: %1: %2").arg(fileName, QString::fromUtf8(error.what()))
        );
        return;
    }
    if (presets.size() == 1) {
        return applyPreset(presets.first());
    }
    QStringList names;
    for (const auto& preset : presets) {
        names << preset.name;
    }
    if (const std::optional<QString> name =
            AskItem(this, tr("Preset"), tr("Pick a preset:"), names)) {
        applyPreset(presets.value(names.indexOf(*name)));
    }
}

void EqualizerWindow::saveEqf() {
    const QString name = AskText(this, tr("Save the preset"), tr("Name:"), QStringLiteral("QiYaa"))
                             .value_or(QString());
    if (name.isEmpty()) {
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save the preset"), QDir::homePath() + u'/' + name + QStringLiteral(".eqf"),
        tr("Winamp preset (*.eqf)")
    );
    if (path.isEmpty()) {
        return;
    }
    const QString fileName = QFileInfo(path).fileName();
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)
        && file.write(Audio::WriteEqf({{name, equalizerSettings}})) > 0) {
        Q_EMIT statusText(tr("EQ: saved to %1").arg(fileName));
    } else {
        Q_EMIT statusText(tr("EQ: %1 is not saved: %2").arg(fileName, file.errorString()));
    }
}

void EqualizerWindow::showPresets() {
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(tr("Reset (0 dB)"), this, [this] {
        const bool enabled = equalizerSettings.enabled;
        equalizerSettings = Audio::EqSettings{};
        equalizerSettings.enabled = enabled;
        Q_EMIT settingsChanged(equalizerSettings);
        update();
    });
    menu->addAction(tr("Load .eqf…"), this, &EqualizerWindow::loadEqf);
    menu->addAction(tr("Save to .eqf…"), this, &EqualizerWindow::saveEqf);
    menu->addSeparator();
    for (const Audio::EqPreset& preset : Audio::BuiltinEqPresets()) {
        menu->addAction(preset.name, this, [this, preset] { applyPreset(preset); });
    }
    const QPoint at(
        Skins::EqualizerSprites::kPresetsPosition.x(),
        Skins::EqualizerSprites::kPresetsPosition.y() + 12
    );
    menu->popup(mapToGlobal(QPoint(qRound(at.x() * scale()), qRound(at.y() * scale()))));
}

void EqualizerWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    Q_EMIT closeRequested();
}

void EqualizerWindow::retranslate() {
    setWindowTitle(tr("QiYaa: equalizer"));
}

}  // namespace Ui
