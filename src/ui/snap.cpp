#include "ui/snap.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>

namespace Ui {
namespace {

// Exclusive edges as in webamp; QRect::right()/bottom() are inclusive (x + w - 1).
int LeftEdge(const QRect& rect) {
    return rect.x();
}
int TopEdge(const QRect& rect) {
    return rect.y();
}
int RightEdge(const QRect& rect) {
    return rect.x() + rect.width();
}
int BottomEdge(const QRect& rect) {
    return rect.y() + rect.height();
}

bool WithinDistance(int edge, int value, int distance) {
    return std::abs(edge - value) < distance;
}

bool OverlapX(const QRect& first, const QRect& second, int distance) {
    return LeftEdge(first) <= RightEdge(second) + distance
        && LeftEdge(second) <= RightEdge(first) + distance;
}
bool OverlapY(const QRect& first, const QRect& second, int distance) {
    return TopEdge(first) <= BottomEdge(second) + distance
        && TopEdge(second) <= BottomEdge(first) + distance;
}

struct Snapped {
    std::optional<int> x;
    std::optional<int> y;
};

Snapped SnapOne(const QRect& moving, const QRect& other, int distance) {
    Snapped snapped;
    if (OverlapY(moving, other, distance)) {
        if (WithinDistance(LeftEdge(moving), RightEdge(other), distance)) {
            snapped.x = RightEdge(other);
        } else if (WithinDistance(RightEdge(moving), LeftEdge(other), distance)) {
            snapped.x = LeftEdge(other) - moving.width();
        } else if (WithinDistance(LeftEdge(moving), LeftEdge(other), distance)) {
            snapped.x = LeftEdge(other);
        } else if (WithinDistance(RightEdge(moving), RightEdge(other), distance)) {
            snapped.x = RightEdge(other) - moving.width();
        }
    }
    if (OverlapX(moving, other, distance)) {
        if (WithinDistance(TopEdge(moving), BottomEdge(other), distance)) {
            snapped.y = BottomEdge(other);
        } else if (WithinDistance(BottomEdge(moving), TopEdge(other), distance)) {
            snapped.y = TopEdge(other) - moving.height();
        } else if (WithinDistance(TopEdge(moving), TopEdge(other), distance)) {
            snapped.y = TopEdge(other);
        } else if (WithinDistance(BottomEdge(moving), BottomEdge(other), distance)) {
            snapped.y = BottomEdge(other) - moving.height();
        }
    }
    return snapped;
}

long long DistanceSquared(const QRect& first, const QRect& second) {
    const long long dx =
        std::max({0, LeftEdge(second) - RightEdge(first), LeftEdge(first) - RightEdge(second)});
    const long long dy =
        std::max({0, TopEdge(second) - BottomEdge(first), TopEdge(first) - BottomEdge(second)});
    return dx * dx + dy * dy;
}

}  // namespace

QPoint SnapToOthers(const QRect& moving, const QList<QRect>& others, int distance) {
    QPoint point = moving.topLeft();
    for (const QRect& other : others) {
        const Snapped snapped = SnapOne(moving, other, distance);
        if (snapped.x) {
            point.setX(*snapped.x);
        }
        if (snapped.y) {
            point.setY(*snapped.y);
        }
    }
    return point;
}

QPoint SnapWithin(const QRect& moving, const QRect& screen, int distance) {
    QPoint point = moving.topLeft();
    if (LeftEdge(moving) - distance < LeftEdge(screen)) {
        point.setX(LeftEdge(screen));
    } else if (RightEdge(moving) + distance > RightEdge(screen)) {
        point.setX(RightEdge(screen) - moving.width());
    }

    if (TopEdge(moving) - distance < TopEdge(screen)) {
        point.setY(TopEdge(screen));
    } else if (BottomEdge(moving) + distance > BottomEdge(screen)) {
        point.setY(BottomEdge(screen) - moving.height());
    }
    return point;
}

QRect PickScreen(const QRect& rect, const QList<QRect>& screens) {
    QRect best;
    long long bestArea = -1;
    for (const QRect& screen : screens) {
        const QRect overlap = screen.intersected(rect);
        const long long area = overlap.isEmpty() ? 0 : 1LL * overlap.width() * overlap.height();
        if (area > bestArea) {
            bestArea = area;
            best = screen;
        }
    }
    if (bestArea > 0 || screens.isEmpty()) {
        return best;
    }

    long long bestDistance = std::numeric_limits<long long>::max();
    for (const QRect& screen : screens) {
        const long long distance = DistanceSquared(rect, screen);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = screen;
        }
    }
    return best;
}

QPoint ClampInside(const QRect& rect, const QRect& screen) {
    if (screen.isEmpty()) {
        return rect.topLeft();
    }
    int x = rect.x();
    int y = rect.y();
    if (RightEdge(rect) > RightEdge(screen)) {
        x = RightEdge(screen) - rect.width();
    }
    if (BottomEdge(rect) > BottomEdge(screen)) {
        y = BottomEdge(screen) - rect.height();
    }
    if (x < LeftEdge(screen)) {
        x = LeftEdge(screen);
    }
    if (y < TopEdge(screen)) {
        y = TopEdge(screen);
    }
    return {x, y};
}

bool Touching(const QRect& first, const QRect& second) {
    const bool yOverlap =
        TopEdge(first) < BottomEdge(second) && TopEdge(second) < BottomEdge(first);
    const bool xOverlap =
        LeftEdge(first) < RightEdge(second) && LeftEdge(second) < RightEdge(first);
    if (yOverlap
        && (RightEdge(first) == LeftEdge(second) || RightEdge(second) == LeftEdge(first))) {
        return true;
    }
    if (xOverlap
        && (BottomEdge(first) == TopEdge(second) || BottomEdge(second) == TopEdge(first))) {
        return true;
    }
    return false;
}

QList<int> ConnectedGroup(int start, const QList<QRect>& rects) {
    QList<int> group;
    QList<int> queue{start};
    QList<bool> seen(rects.size(), false);
    if (start < 0 || start >= rects.size()) {
        return group;
    }
    seen[start] = true;
    while (!queue.isEmpty()) {
        const int current = queue.takeFirst();
        for (int i = 0; i < rects.size(); ++i) {
            if (seen[i] || !Touching(rects[current], rects[i])) {
                continue;
            }
            seen[i] = true;
            group << i;
            queue << i;
        }
    }
    return group;
}

QList<int> StackBelow(int self, const QList<QRect>& rects, int dy, const QList<bool>& solid) {
    if (self < 0 || self >= rects.size() || dy == 0) {
        return {};
    }
    const auto isSolid = [&](int i) { return solid.isEmpty() || solid.value(i, true); };
    const QRect oldRect = rects[self];
    const QRect grown = oldRect.adjusted(0, 0, 0, dy);
    const auto hangsFrom = [](const QRect& upper, const QRect& lower) {
        return BottomEdge(upper) == TopEdge(lower) && LeftEdge(upper) < RightEdge(lower)
            && LeftEdge(lower) < RightEdge(upper);
    };
    const int count = static_cast<int>(rects.size());
    QList<bool> blocked(count, false);
    for (;;) {
        QList<bool> moving(count, false);
        for (bool changed = true; changed;) {
            changed = false;
            for (int i = 0; i < count; ++i) {
                if (i == self || moving[i] || blocked[i]
                    || TopEdge(rects[i]) < BottomEdge(oldRect)) {
                    continue;
                }
                bool follows = hangsFrom(oldRect, rects[i]);
                for (int j = 0; j < count && !follows; ++j) {
                    follows = moving[j] && hangsFrom(rects[j], rects[i]);
                }
                if (dy > 0) {
                    follows = follows || grown.intersects(rects[i]);
                    for (int j = 0; j < count && !follows; ++j) {
                        follows = moving[j] && rects[j].translated(0, dy).intersects(rects[i]);
                    }
                } else if (follows) {
                    for (int j = 0; j < count; ++j) {
                        if (j != self && j != i && !moving[j] && isSolid(j)
                            && hangsFrom(rects[j], rects[i])) {
                            follows = false;
                        }
                    }
                }
                if (follows) {
                    moving[i] = changed = true;
                }
            }
        }
        bool conflict = false;
        if (dy < 0) {
            for (int i = 0; i < count; ++i) {
                if (!moving[i]) {
                    continue;
                }
                const QRect moved = rects[i].translated(0, dy);
                for (int j = 0; j < count; ++j) {
                    if (j != self && j != i && !moving[j] && isSolid(j)
                        && moved.intersects(rects[j])) {
                        blocked[i] = conflict = true;
                    }
                }
            }
        }
        if (!conflict) {
            QList<int> followers;
            for (int i = 0; i < count; ++i) {
                if (moving[i]) {
                    followers << i;
                }
            }
            return followers;
        }
    }
}

QPoint ResolveDragPosition(
    const QRect& proposed,
    const QList<QRect>& others,
    const QList<QRect>& screens,
    int distance
) {
    QRect rect = proposed;
    rect.moveTopLeft(SnapToOthers(rect, others, distance));
    const QRect screen = PickScreen(rect, screens);
    if (screen.isEmpty()) {
        return rect.topLeft();
    }
    rect.moveTopLeft(SnapWithin(rect, screen, distance));
    return ClampInside(rect, screen);
}

}  // namespace Ui
