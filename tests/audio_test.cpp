#include "audio/audio_engine.h"
#include "core/player.h"
#include "yandex/api_client.h"
#include "yandex/library.h"

#include <QElapsedTimer>
#include <QFile>
#include <QIODevice>
#include <QLatin1String>
#include <QMetaEnum>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QTest>

#include <functional>

using Audio::AudioEngine;

class TestAudio : public QObject {
    Q_OBJECT
private:
    Audio::AudioEngine engine;
    QByteArray mp3;

    QString describeEngine(qsizetype advancedCount, qsizetype finishedCount = -1) const {
        return QStringLiteral("advanced %1, finished %2, at %3 s, %4, current %5, queued %6")
            .arg(advancedCount)
            .arg(finishedCount)
            .arg(engine.positionSeconds(), 0, 'f', 2)
            .arg(QLatin1String(QMetaEnum::fromType<Audio::AudioEngine::State>().valueToKey(
                static_cast<int>(engine.state())
            )))
            .arg(engine.currentStream())
            .arg(engine.queuedStream());
    }

    void pumpUntil(const std::function<bool()>& condition, int timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < timeoutMs) {
            engine.poll();
            QTest::qWait(20);
        }
    }

    Audio::AudioEngine::TStreamId startNearEnd(double at = 2.2) {
        const auto stream = engine.beginStream();
        engine.appendData(stream, mp3);
        engine.finishData(stream);
        pumpUntil([&] { return engine.state() == Audio::AudioEngine::State::Playing; }, 3000);
        engine.seek(at);
        pumpUntil([&] { return engine.positionSeconds() >= at; }, 2000);
        return stream;
    }

    Audio::AudioEngine::TStreamId queueWhole(const QByteArray& data) {
        const auto stream = engine.queueStream();
        engine.appendData(stream, data);
        engine.finishData(stream);
        return stream;
    }

private Q_SLOTS:
    void initTestCase() {
        if (const Audio::AudioEngine::InitResult initResult = engine.init(); !initResult.ok) {
            QSKIP(qPrintable("no audio output: " + initResult.message));
        }
        engine.setVolume(0);
        QFile file(QStringLiteral(QIYAA_TEST_DATA "/sine440_3s.mp3"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        mp3 = file.readAll();
        qInfo("backend: %s", qPrintable(engine.backendName()));
    }

    void streamsInChunksAndPlays() {
        QSignalSpy finished(&engine, &Audio::AudioEngine::trackFinished);
        engine.beginStream();
        QCOMPARE(engine.state(), Audio::AudioEngine::State::Buffering);

        for (qsizetype offset = 0; offset < mp3.size(); offset += 4096) {
            engine.appendData(mp3.mid(offset, 4096));
            engine.poll();
            QTest::qWait(10);
        }
        engine.finishData();

        pumpUntil([&] { return engine.state() == Audio::AudioEngine::State::Playing; }, 3000);
        QCOMPARE(engine.state(), Audio::AudioEngine::State::Playing);
        QCOMPARE(engine.sourceSampleRate(), 44'100);
        QCOMPARE(engine.sourceChannels(), 2);

        const double startPosition = engine.positionSeconds();
        QTest::qWait(500);
        QVERIFY2(engine.positionSeconds() > startPosition + 0.2, "position does not advance");

        QVERIFY(engine.seek(2.5));
        pumpUntil([&] { return finished.count() > 0; }, 4000);
        QCOMPARE(finished.count(), 1);
        QVERIFY(engine.positionSeconds() >= 2.5);
    }

    void pauseStopsTheClock() {
        engine.beginStream();
        engine.appendData(mp3);
        engine.finishData();
        pumpUntil([&] { return engine.state() == Audio::AudioEngine::State::Playing; }, 3000);
        engine.pause();
        QCOMPARE(engine.state(), Audio::AudioEngine::State::Paused);
        const double pausedPosition = engine.positionSeconds();
        QTest::qWait(300);
        QCOMPARE(engine.positionSeconds(), pausedPosition);
        engine.resume();
        QCOMPARE(engine.state(), Audio::AudioEngine::State::Playing);
        engine.stop();
        QCOMPARE(engine.state(), Audio::AudioEngine::State::Stopped);
    }

    void garbageReportsError() {
        QSignalSpy errors(&engine, &Audio::AudioEngine::errorOccurred);
        engine.beginStream();
        engine.appendData(QByteArray(20'000, 'x'));
        engine.finishData();
        pumpUntil([&] { return errors.count() > 0; }, 3000);
        QCOMPARE(errors.count(), 1);
        const QString message = errors.first().first().toString();
        QVERIFY2(message.contains(QStringLiteral("20000 bytes")), qPrintable(message));
        QCOMPARE(engine.state(), Audio::AudioEngine::State::Stopped);
    }

    void rapidSeeksWhileDownloadingLandOnTheLastTarget() {
        // ~39 s stream: the 3 s file 13 times over (MP3 frames concatenate).
        QByteArray longMp3;
        for (int i = 0; i < 13; ++i) {
            longMp3 += mp3;
        }
        engine.beginStream();
        qsizetype fed = 0;
        auto feed = [&](qsizetype byteCount) {
            engine.appendData(longMp3.mid(fed, byteCount));
            fed += byteCount;
        };
        feed(64 * 1024);
        pumpUntil([&] { return engine.state() == Audio::AudioEngine::State::Playing; }, 3000);
        QCOMPARE(engine.state(), Audio::AudioEngine::State::Playing);
        const double targets[] = {1.0, 30.0, 2.0, 35.0, 0.5, 20.0, 3.0, 10.0};
        for (int round = 0; round < 3; ++round) {
            for (double target : targets) {
                QVERIFY(engine.seek(target));
                QTest::qWait(5);
                if (fed < longMp3.size()) {
                    feed(16 * 1024);
                }
                engine.poll();
            }
        }
        feed(longMp3.size() - fed);
        engine.finishData();
        QVERIFY(engine.seek(12.0));
        pumpUntil([&] { return engine.positionSeconds() > 12.2; }, 3000);
        const double position = engine.positionSeconds();
        QVERIFY2(position >= 12.0 && position < 14.0, qPrintable(QString::number(position)));
        engine.stop();
    }

    void playerPollsTheEngine() {
        Yandex::ApiClient api(nullptr);
        Yandex::Library library(&api);
        Core::Player player(&library, &engine);
        QSignalSpy finished(&engine, &Audio::AudioEngine::trackFinished);
        engine.beginStream();
        engine.appendData(mp3);
        engine.finishData();
        QVERIFY(QTest::qWaitFor(
            [&] { return engine.state() == Audio::AudioEngine::State::Playing; }, 3000
        ));
        QVERIFY(engine.seek(2.7));
        QVERIFY(finished.wait(4000));
    }

    void restartingWhileStreamingDoesNotHang() {
        for (int i = 0; i < 5; ++i) {
            engine.beginStream();
            engine.appendData(mp3.left(8000));
            QTest::qWait(30);
        }
        engine.stop();
        QCOMPARE(engine.state(), Audio::AudioEngine::State::Stopped);
    }

    void queuedStreamFollowsWithoutGap() {
        QSignalSpy finished(&engine, &Audio::AudioEngine::trackFinished);
        QSignalSpy advanced(&engine, &Audio::AudioEngine::trackAdvanced);
        startNearEnd();
        QSignalSpy states(&engine, &Audio::AudioEngine::stateChanged);
        const auto nextStream = queueWhole(mp3);
        QVERIFY(nextStream != 0);
        QCOMPARE(engine.queuedStream(), nextStream);
        pumpUntil([&] { return advanced.count() > 0 || finished.count() > 0; }, 3000);
        QVERIFY2(
            advanced.count() == 1, qPrintable(describeEngine(advanced.count(), finished.count()))
        );
        QCOMPARE(finished.count(), 0);
        QVERIFY(states.isEmpty());
        QCOMPARE(engine.currentStream(), nextStream);
        QCOMPARE(engine.queuedStream(), static_cast<Audio::AudioEngine::TStreamId>(0));
        QVERIFY2(
            engine.positionSeconds() < 0.3, qPrintable(QString::number(engine.positionSeconds()))
        );
        QVERIFY(engine.seek(2.7));
        pumpUntil([&] { return finished.count() > 0; }, 3000);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(advanced.count(), 1);
        engine.stop();
    }

    void seekingBackUndoesTheChain() {
        QSignalSpy advanced(&engine, &Audio::AudioEngine::trackAdvanced);
        startNearEnd(2.3);
        const auto nextStream = queueWhole(mp3);
        QTest::qWait(150);  // the rest of track 1 is decoded and track 2 chained behind it
        QVERIFY(engine.seek(0.5));
        pumpUntil(
            [&] {
                const double position = engine.positionSeconds();
                return position >= 0.8 && position < 1.5;
            },
            2000
        );
        QCOMPARE(advanced.count(), 0);
        const double position = engine.positionSeconds();
        QVERIFY2(position >= 0.5 && position < 1.5, qPrintable(QString::number(position)));
        QCOMPARE(engine.queuedStream(), nextStream);
        QVERIFY(engine.seek(2.5));
        pumpUntil([&] { return advanced.count() > 0; }, 3000);
        QVERIFY2(advanced.count() == 1, qPrintable(describeEngine(advanced.count())));
        QVERIFY(engine.positionSeconds() < 0.3);
        engine.stop();
    }

    void clearingAChainedStreamStopsAtTheBoundary() {
        QSignalSpy finished(&engine, &Audio::AudioEngine::trackFinished);
        QSignalSpy advanced(&engine, &Audio::AudioEngine::trackAdvanced);
        startNearEnd(2.3);
        queueWhole(mp3);
        QTest::qWait(150);  // chained
        engine.clearQueued();
        pumpUntil([&] { return finished.count() > 0 || advanced.count() > 0; }, 3000);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(advanced.count(), 0);
        engine.stop();
    }

    void playQueuedNowJumpsImmediately() {
        startNearEnd(0.5);
        const auto nextStream = queueWhole(mp3);
        QCOMPARE(engine.playQueuedNow(), nextStream);
        QCOMPARE(engine.currentStream(), nextStream);
        pumpUntil([&] { return engine.state() == Audio::AudioEngine::State::Playing; }, 3000);
        QVERIFY(engine.positionSeconds() < 0.5);
        QCOMPARE(engine.queuedStream(), static_cast<Audio::AudioEngine::TStreamId>(0));
        engine.stop();
    }

    void undecodableQueuedStreamIsSkipped() {
        QSignalSpy finished(&engine, &Audio::AudioEngine::trackFinished);
        QSignalSpy advanced(&engine, &Audio::AudioEngine::trackAdvanced);
        startNearEnd(2.3);
        queueWhole(QByteArray(100'000, 'x'));
        pumpUntil([&] { return finished.count() > 0 || advanced.count() > 0; }, 3000);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(advanced.count(), 0);
        QCOMPARE(engine.queuedStream(), static_cast<Audio::AudioEngine::TStreamId>(0));
        engine.stop();
    }

    void queuedStreamArrivingLateStillChains() {
        QSignalSpy finished(&engine, &Audio::AudioEngine::trackFinished);
        QSignalSpy advanced(&engine, &Audio::AudioEngine::trackAdvanced);
        startNearEnd(2.0);
        const auto nextStream = engine.queueStream();
        engine.appendData(nextStream, mp3.left(4000));  // not enough to start decoding it yet
        QTest::qWait(200);
        engine.appendData(nextStream, mp3.mid(4000));
        engine.finishData(nextStream);
        pumpUntil([&] { return finished.count() > 0 || advanced.count() > 0; }, 3000);
        QVERIFY2(
            advanced.count() == 1, qPrintable(describeEngine(advanced.count(), finished.count()))
        );
        QCOMPARE(finished.count(), 0);
        engine.stop();
    }

    void partlyDownloadedQueuedStreamIsNotChained() {
        QSignalSpy finished(&engine, &Audio::AudioEngine::trackFinished);
        QSignalSpy advanced(&engine, &Audio::AudioEngine::trackAdvanced);
        startNearEnd(2.4);
        const auto nextStream = engine.queueStream();
        engine.appendData(nextStream, mp3.left(mp3.size() / 2));
        pumpUntil([&] { return finished.count() > 0 || advanced.count() > 0; }, 3000);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(advanced.count(), 0);
        QCOMPARE(engine.playQueuedNow(), nextStream);
        engine.appendData(nextStream, mp3.mid(mp3.size() / 2));
        engine.finishData(nextStream);
        pumpUntil([&] { return engine.state() == Audio::AudioEngine::State::Playing; }, 3000);
        QCOMPARE(engine.state(), Audio::AudioEngine::State::Playing);
        engine.stop();
    }

    void clearingAChainedStreamThenRestartingDoesNotHang() {
        for (int round = 0; round < 2; ++round) {
            startNearEnd(2.3);
            queueWhole(mp3);
            QTest::qWait(150);  // chained
            engine.clearQueued();
            QElapsedTimer timer;
            timer.start();
            if (round == 0) {
                engine.stop();
            } else {
                engine.beginStream();
            }
            QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
        }
        engine.stop();
    }

    void stopReturnsPromptlyWhileQueuedDataIsMissing() {
        startNearEnd(2.9);
        const auto nextStream = engine.queueStream();
        engine.appendData(nextStream, mp3.left(70'000 < mp3.size() ? 70'000 : mp3.size() / 2));
        QTest::qWait(300);
        QElapsedTimer timer;
        timer.start();
        engine.stop();
        QVERIFY(timer.elapsed() < 1000);
        QCOMPARE(engine.state(), Audio::AudioEngine::State::Stopped);
    }
};

QTEST_GUILESS_MAIN(TestAudio)
#include "audio_test.moc"
