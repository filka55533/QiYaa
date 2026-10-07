#include "audio/equalizer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>

namespace Audio {

namespace {
constexpr double kQ = 1.2;
constexpr int kDirty = 4;
}  // namespace

EqualizerDsp::EqualizerDsp() {
    coefficientSlots[front] = ComputeCoefficients(lastSettings, sampleRate);
}

void EqualizerDsp::setSampleRate(uint32_t rate) {
    sampleRate = rate > 0 ? rate : 44'100;
    publish(lastSettings);
    // No audio thread yet: make it current now.
    const int previousMiddle = middle.exchange(front);
    if (previousMiddle & kDirty) {
        front = previousMiddle & 3;
    }
}

EqualizerDsp::Coefficients
EqualizerDsp::ComputeCoefficients(const EqSettings& settings, double sampleRate) {
    Coefficients coefficients;
    coefficients.enabled = settings.enabled;
    coefficients.preamp =
        static_cast<float>(std::pow(10.0, std::clamp(settings.preampDb, -kEqMaxDb, kEqMaxDb) / 20.0)
        );
    for (int i = 0; i < kEqBands; ++i) {
        Biquad& filter = coefficients.bands[i];
        const double db = std::clamp(settings.bandsDb[i], -kEqMaxDb, kEqMaxDb);
        const double f0 = kEqBandHz[i];
        if (std::abs(db) < 0.05 || f0 >= sampleRate * 0.49) {
            continue;
        }
        // RBJ audio EQ cookbook, peaking EQ.
        const double A = std::pow(10.0, db / 40.0);
        const double w0 = 2.0 * std::numbers::pi * f0 / sampleRate;
        const double alpha = std::sin(w0) / (2.0 * kQ);
        const double cw = std::cos(w0);
        const double a0 = 1.0 + alpha / A;
        filter.b0 = static_cast<float>((1.0 + alpha * A) / a0);
        filter.b1 = static_cast<float>((-2.0 * cw) / a0);
        filter.b2 = static_cast<float>((1.0 - alpha * A) / a0);
        filter.a1 = static_cast<float>((-2.0 * cw) / a0);
        filter.a2 = static_cast<float>((1.0 - alpha / A) / a0);
        filter.identity = false;
    }
    return coefficients;
}

void EqualizerDsp::publish(const EqSettings& settings) {
    lastSettings = settings;
    coefficientSlots[back] = ComputeCoefficients(settings, sampleRate);
    const int previousMiddle = middle.exchange(back | kDirty, std::memory_order_acq_rel);
    back = previousMiddle & 3;
}

void EqualizerDsp::process(std::span<float> stereoFrames) {
    const size_t frameCount = stereoFrames.size() / 2;
    float* frames = stereoFrames.data();
    if (middle.load(std::memory_order_relaxed) & kDirty) {
        const int previousMiddle = middle.exchange(front, std::memory_order_acq_rel);
        front = previousMiddle & 3;
    }
    const Coefficients& coefficients = coefficientSlots[front];
    if (!coefficients.enabled) {
        return;
    }

    if (coefficients.preamp != 1.0f) {
        for (size_t i = 0; i < frameCount * 2; ++i) {
            frames[i] *= coefficients.preamp;
        }
    }

    for (int band = 0; band < kEqBands; ++band) {
        const Biquad& filter = coefficients.bands[band];
        if (filter.identity) {
            filterState1[band] = {0, 0};
            filterState2[band] = {0, 0};
            continue;
        }
        for (int channel = 0; channel < 2; ++channel) {
            float z1 = filterState1[band][channel], z2 = filterState2[band][channel];
            float* sample = frames + channel;
            for (size_t i = 0; i < frameCount; ++i, sample += 2) {
                // Transposed direct form II.
                const float x = *sample;
                const float y = filter.b0 * x + z1;
                z1 = filter.b1 * x - filter.a1 * y + z2;
                z2 = filter.b2 * x - filter.a2 * y;
                *sample = y;
            }
            // Flush denormals.
            filterState1[band][channel] = std::abs(z1) < 1e-15f ? 0.0f : z1;
            filterState2[band][channel] = std::abs(z2) < 1e-15f ? 0.0f : z2;
        }
    }
}

double EqualizerDsp::ResponseDb(const EqSettings& settings, double hz, double sampleRate) {
    const Coefficients coefficients = ComputeCoefficients(settings, sampleRate);
    if (!coefficients.enabled) {
        return 0.0;
    }
    const std::complex<double> z =
        std::polar(1.0, -2.0 * std::numbers::pi * hz / sampleRate);  // z^-1
    std::complex<double> h = coefficients.preamp;
    for (const Biquad& filter : coefficients.bands) {
        if (filter.identity) {
            continue;
        }
        h *= (static_cast<double>(filter.b0) + static_cast<double>(filter.b1) * z
              + static_cast<double>(filter.b2) * z * z)
            / (1.0 + static_cast<double>(filter.a1) * z + static_cast<double>(filter.a2) * z * z);
    }
    return 20.0 * std::log10(std::abs(h));
}

}  // namespace Audio
