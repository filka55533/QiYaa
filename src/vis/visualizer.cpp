#include "vis/visualizer.h"

#include "skins/skin.h"

#include <QColor>
#include <QCoreApplication>
#include <QPainter>
#include <QRect>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace Vis {

namespace {

QColor VisColor(const Skins::Skin& skin, int index) {
    const auto& colors = skin.visColors();
    return index < colors.size() ? colors[index] : QColor(Qt::green);
}

class Spectrum : public Visualizer {
public:
    static constexpr int kBars = kSpectrumBars;

    QString name() const override {
        return QCoreApplication::translate("Vis::Visualizer", "Spectrum");
    }

    void reset() override {
        bars.fill(0);
        peaks.fill(0);
        peakFrames.fill(0);
    }

    void update(const VisFrame& frame) override {
        const std::array<float, kBars> levels =
            SpectrumLevels(frame.spectrum, frame.sampleRate, frame.fftSize);
        for (int bar = 0; bar < kBars; ++bar) {
            bars[bar] = std::max(levels[bar], bars[bar] - 0.07f);
            float peak = peaks[bar] - 0.0004f * peakFrames[bar] * peakFrames[bar];
            if (peak < bars[bar]) {
                peak = bars[bar];
                peakFrames[bar] = 0;
            } else {
                ++peakFrames[bar];
            }
            peaks[bar] = std::max(peak, 0.0f);
        }
    }

    void render(QPainter& painter, const QRect& area, const Skins::Skin& skin) const override {
        const int areaHeight = area.height();
        for (int bar = 0; bar < kBars; ++bar) {
            const int x = area.x() + bar * 4;
            const int barHeight = static_cast<int>(std::ceil(bars[bar] * areaHeight));
            for (int i = 0; i < barHeight; ++i) {
                const int colorIndex = 2 + (areaHeight - 1 - i) * 16 / areaHeight;
                painter.fillRect(
                    x, area.y() + areaHeight - 1 - i, 3, 1, VisColor(skin, colorIndex)
                );
            }
            const int peakHeight = static_cast<int>(std::ceil(peaks[bar] * areaHeight));
            if (peakHeight > 0) {
                painter.fillRect(x, area.y() + areaHeight - peakHeight, 3, 1, VisColor(skin, 23));
            }
        }
    }

private:
    std::array<float, kBars> bars{};
    std::array<float, kBars> peaks{};
    std::array<int, kBars> peakFrames{};
};

class Oscilloscope : public Visualizer {
public:
    QString name() const override {
        return QCoreApplication::translate("Vis::Visualizer", "Oscilloscope");
    }
    void reset() override { rows.fill(-1); }

    void update(const VisFrame& frame) override {
        const int sampleCount = static_cast<int>(frame.left.size());
        // Winamp shows ~576 samples across the 75 px area.
        const int shownSamples = std::min(sampleCount, 576);
        const int start = sampleCount - shownSamples;
        for (int x = 0; x < kWidth; ++x) {
            const int sample = start + x * shownSamples / kWidth;
            const float mono = 0.5f * (frame.left[sample] + frame.right[sample]);
            rows[x] = std::clamp(static_cast<int>(std::lround(7.5f - mono * 8.0f)), 0, 15);
        }
    }

    void render(QPainter& painter, const QRect& area, const Skins::Skin& skin) const override {
        if (rows[0] < 0) {
            return;
        }
        int previousY = rows[0];
        for (int x = 0; x < kWidth && x < area.width(); ++x) {
            const int y = rows[x];
            const int top = std::min(previousY, y), bottom = std::max(previousY, y);
            for (int row = top; row <= bottom; ++row) {
                const int distance = static_cast<int>(std::abs(row - 7.5f));
                painter.fillRect(
                    area.x() + x, area.y() + row, 1, 1,
                    VisColor(skin, 18 + std::min(4, distance / 2))
                );
            }
            previousY = y;
        }
    }

private:
    static constexpr int kWidth = 75;
    std::array<int, kWidth> rows = [] {
        std::array<int, kWidth> initialRows{};
        initialRows.fill(-1);
        return initialRows;
    }();
};

}  // namespace

Analyzer::Analyzer(int fftSize)
    : fftLength(fftSize)
    , windowFunction(fftSize)
    , real(fftSize)
    , imaginary(fftSize)
    , decibels(fftSize / 2 + 1) {
    for (int i = 0; i < fftSize; ++i) {
        windowFunction[i] =
            0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * i / (fftSize - 1));
    }
}

const std::vector<float>& Analyzer::analyze(std::span<const float> mono) {
    const int n = fftLength;
    for (int i = 0; i < n; ++i) {
        real[i] = i < static_cast<int>(mono.size()) ? mono[i] * windowFunction[i] : 0.0f;
        imaginary[i] = 0.0f;
    }
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imaginary[i], imaginary[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * std::numbers::pi_v<float> / len;
        const float wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const float tr = real[b] * cr - imaginary[b] * ci;
                const float ti = real[b] * ci + imaginary[b] * cr;
                real[b] = real[a] - tr;
                imaginary[b] = imaginary[a] - ti;
                real[a] += tr;
                imaginary[a] += ti;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
    // A full-scale sine gives magnitude n/4 with a Hann window -> 0 dBFS.
    const float magnitudeScale = 4.0f / n;
    for (int i = 0; i <= n / 2; ++i) {
        const float magnitude =
            std::sqrt(real[i] * real[i] + imaginary[i] * imaginary[i]) * magnitudeScale;
        decibels[i] = 20.0f * std::log10(std::max(magnitude, 1e-9f));
    }
    return decibels;
}

std::array<SpectrumBand, kSpectrumBars> SpectrumBands(int sampleRate, int fftSize) {
    const int binCount = fftSize / 2 + 1;
    const double binHz = static_cast<double>(sampleRate) / fftSize;
    const double lowestHz = 60.0, highestHz = std::min(16'000.0, sampleRate / 2.0);
    std::array<SpectrumBand, kSpectrumBars> bands{};
    for (int bar = 0; bar < kSpectrumBars; ++bar) {
        SpectrumBand& band = bands[bar];
        band.lowHz =
            lowestHz * std::pow(highestHz / lowestHz, static_cast<double>(bar) / kSpectrumBars);
        band.highHz =
            lowestHz * std::pow(highestHz / lowestHz, static_cast<double>(bar + 1) / kSpectrumBars);
        band.firstBin = std::clamp(static_cast<int>(band.lowHz / binHz), 1, binCount - 1);
        band.endBin = std::clamp(
            static_cast<int>(std::ceil(band.highHz / binHz)), band.firstBin + 1, binCount
        );
    }
    return bands;
}

std::array<float, kSpectrumBars>
SpectrumLevels(std::span<const float> spectrumDb, int sampleRate, int fftSize) {
    constexpr float kMinDb = -72.0f, kMaxDb = -6.0f;
    std::array<float, kSpectrumBars> levels{};
    const std::array<SpectrumBand, kSpectrumBars> bands = SpectrumBands(sampleRate, fftSize);
    for (int bar = 0; bar < kSpectrumBars; ++bar) {
        float peakDb = kMinDb;
        const int endBin = std::min(bands[bar].endBin, static_cast<int>(spectrumDb.size()));
        for (int i = bands[bar].firstBin; i < endBin; ++i) {
            peakDb = std::max(peakDb, spectrumDb[i]);
        }
        levels[bar] = std::clamp((peakDb - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);
    }
    return levels;
}

std::unique_ptr<Visualizer> MakeSpectrum() {
    return std::make_unique<Spectrum>();
}
std::unique_ptr<Visualizer> MakeOscilloscope() {
    return std::make_unique<Oscilloscope>();
}

}  // namespace Vis
