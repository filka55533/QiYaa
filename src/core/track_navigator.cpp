#include "core/track_navigator.h"

#include <QList>
#include <QRandomGenerator>

#include <algorithm>

namespace Core {
namespace {

class SequentialNavigator : public TrackNavigator {
public:
    void reset(int size, int current) override {
        trackCount = size;
        select(current);
    }
    void select(int index) override { currentIndex = index; }
    int next(bool repeat) const override {
        if (trackCount == 0) {
            return -1;
        }
        return currentIndex + 1 < trackCount ? currentIndex + 1 : (repeat ? 0 : -1);
    }
    int previous(bool repeat) const override {
        if (trackCount == 0) {
            return -1;
        }
        return currentIndex > 0 ? currentIndex - 1 : (repeat ? trackCount - 1 : 0);
    }

protected:
    int trackCount = 0;
    int currentIndex = -1;
};

class RandomNavigator final : public SequentialNavigator {
public:
    void select(int index) override {
        SequentialNavigator::select(index);
        nextIndex = -1;
        if (trackCount < 2) {
            return;
        }
        nextIndex = static_cast<int>(QRandomGenerator::global()->bounded(trackCount - 1));
        if (nextIndex >= currentIndex) {
            ++nextIndex;
        }
    }
    int next(bool repeat) const override {
        return trackCount < 2 ? SequentialNavigator::next(repeat) : nextIndex;
    }

private:
    int nextIndex = -1;
};

class ShuffledNavigator final : public TrackNavigator {
public:
    void reset(int size, int current) override {
        order.clear();
        position = -1;
        if (size == 0 || current < 0) {
            return;
        }
        order.reserve(size);
        order << current;
        for (int index = 0; index < size; ++index) {
            if (index != current) {
                order << index;
            }
        }
        std::shuffle(order.begin() + 1, order.end(), *QRandomGenerator::global());
        position = 0;
    }
    void select(int index) override { position = static_cast<int>(order.indexOf(index)); }
    int next(bool repeat) const override {
        if (order.isEmpty()) {
            return -1;
        }
        return position + 1 < order.size() ? order[position + 1] : (repeat ? order.front() : -1);
    }
    int previous(bool repeat) const override {
        if (order.isEmpty()) {
            return -1;
        }
        return position > 0 ? order[position - 1] : (repeat ? order.back() : order.front());
    }

private:
    QList<int> order;
    int position = -1;
};

}  // namespace

std::unique_ptr<TrackNavigator> MakeTrackNavigator(std::optional<ShuffleAlgorithm> algorithm) {
    if (!algorithm) {
        return std::make_unique<SequentialNavigator>();
    }
    if (*algorithm == ShuffleAlgorithm::Random) {
        return std::make_unique<RandomNavigator>();
    }
    return std::make_unique<ShuffledNavigator>();
}

}  // namespace Core
