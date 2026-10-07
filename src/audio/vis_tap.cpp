#include "audio/vis_tap.h"

#include <algorithm>
#include <cstddef>

namespace Audio {

void VisTap::write(std::span<const float> stereoFrames) {
    const size_t frameCount = stereoFrames.size() / 2;
    uint32_t position = writePosition.load(std::memory_order_relaxed);
    for (size_t i = 0; i < frameCount; ++i, ++position) {
        const uint32_t index = position & (kSize - 1);
        leftSamples[index] = stereoFrames[i * 2];
        rightSamples[index] = stereoFrames[i * 2 + 1];
    }
    writePosition.store(position, std::memory_order_release);
}

void VisTap::read(std::span<float> left, std::span<float> right) const {
    const auto count =
        static_cast<uint32_t>(std::min({left.size(), right.size(), static_cast<size_t>(kSize)}));
    const uint32_t end = writePosition.load(std::memory_order_acquire);
    const uint32_t start = end - count;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t index = (start + i) & (kSize - 1);
        left[i] = leftSamples[index];
        right[i] = rightSamples[index];
    }
}

VisReadResult VisTap::readNew(uint32_t cursor, std::span<float> stereo) const {
    const uint32_t end = writePosition.load(std::memory_order_acquire);
    uint32_t count = std::min(end - cursor, kSize);  // unsigned difference survives wrap-around
    count = std::min(count, static_cast<uint32_t>(stereo.size() / 2));
    const uint32_t start = end - count;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t index = (start + i) & (kSize - 1);
        stereo[i * 2] = leftSamples[index];
        stereo[i * 2 + 1] = rightSamples[index];
    }
    return {count, end};
}

void VisTap::clear() {
    leftSamples.fill(0);
    rightSamples.fill(0);
}

}  // namespace Audio
