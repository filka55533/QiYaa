#include "core/track_navigator.h"

#include <QList>
#include <QObject>
#include <QTest>

#include <optional>

class TestTrackNavigator : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void sequentialStopsOrWrapsAtTheBoundary() {
        auto navigator = Core::MakeTrackNavigator(std::nullopt);
        navigator->reset(4, 1);
        QCOMPARE(navigator->next(false), 2);
        QCOMPARE(navigator->previous(false), 0);
        navigator->select(3);
        QCOMPARE(navigator->next(false), -1);
        QCOMPARE(navigator->next(true), 0);
        navigator->select(0);
        QCOMPARE(navigator->previous(false), 0);
        QCOMPARE(navigator->previous(true), 3);
    }

    void randomLookaheadIsStableAndExcludesTheCurrentTrack() {
        auto navigator = Core::MakeTrackNavigator(Core::ShuffleAlgorithm::Random);
        navigator->reset(8, 3);
        for (int transition = 0; transition < 20; ++transition) {
            const int next = navigator->next(false);
            QVERIFY(next >= 0 && next < 8);
            QCOMPARE(navigator->next(false), next);
            QCOMPARE(navigator->next(true), next);
            navigator->select(next);
            QVERIFY(navigator->next(false) != next);
            QCOMPARE(navigator->previous(false), next > 0 ? next - 1 : 0);
        }
        navigator->reset(2, 0);
        QCOMPARE(navigator->next(false), 1);
        navigator->select(1);
        QCOMPARE(navigator->next(false), 0);
    }

    void shuffledLookaheadDoesNotConsumeEntries() {
        auto navigator = Core::MakeTrackNavigator(Core::ShuffleAlgorithm::WithoutRepeats);
        navigator->reset(8, 3);
        QList<int> order{3};
        for (int count = 1; count < 8; ++count) {
            const int next = navigator->next(false);
            QVERIFY(next >= 0 && next < 8);
            QVERIFY(!order.contains(next));
            QCOMPARE(navigator->next(false), next);
            order << next;
            navigator->select(next);
        }
        QCOMPARE(navigator->next(false), -1);
        QCOMPARE(navigator->next(true), 3);
        for (int position = 6; position >= 0; --position) {
            QCOMPARE(navigator->previous(false), order[position]);
            navigator->select(order[position]);
        }
        QCOMPARE(navigator->previous(false), 3);
        QCOMPARE(navigator->previous(true), order.back());
        navigator->select(order[4]);
        QCOMPARE(navigator->next(false), order[5]);
    }

    void emptyAndSingleTrackQueues_data() {
        QTest::addColumn<int>("algorithm");
        QTest::newRow("sequential") << -1;
        QTest::newRow("random") << static_cast<int>(Core::ShuffleAlgorithm::Random);
        QTest::newRow("shuffled") << static_cast<int>(Core::ShuffleAlgorithm::WithoutRepeats);
    }
    void emptyAndSingleTrackQueues() {
        QFETCH(int, algorithm);
        auto navigator = Core::MakeTrackNavigator(
            algorithm < 0 ? std::nullopt
                          : std::optional(static_cast<Core::ShuffleAlgorithm>(algorithm))
        );
        navigator->reset(0, -1);
        QCOMPARE(navigator->next(false), -1);
        QCOMPARE(navigator->next(true), -1);
        QCOMPARE(navigator->previous(true), -1);
        navigator->reset(1, 0);
        QCOMPARE(navigator->next(false), -1);
        QCOMPARE(navigator->next(true), 0);
        QCOMPARE(navigator->previous(false), 0);
    }
};

QTEST_GUILESS_MAIN(TestTrackNavigator)
#include "track_navigator_test.moc"
