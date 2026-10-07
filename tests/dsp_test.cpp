#include "audio/eq_presets.h"
#include "audio/equalizer.h"
#include "audio/error.h"
#include "skins/skin.h"
#include "support/spec_fixtures.h"
#include "ui/equalizer_window.h"
#include "vis/visualizer.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPainter>
#include <QRgb>
#include <QString>
#include <QTest>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace {
std::vector<float> StereoSine(double hz, double sampleRate, int frames, float amplitude = 0.5f) {
    std::vector<float> samples(frames * 2);
    for (int i = 0; i < frames; ++i) {
        const float sample =
            amplitude * static_cast<float>(std::sin(2 * std::numbers::pi * hz * i / sampleRate));
        samples[i * 2] = samples[i * 2 + 1] = sample;
    }
    return samples;
}

double Rms(const std::vector<float>& samples, int fromFrame) {
    double sum = 0;
    int frameCount = 0;
    for (size_t i = fromFrame * 2; i < samples.size(); i += 2, ++frameCount) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    return std::sqrt(sum / frameCount);
}

// spec/dsp: the reference vectors shared with the Android app. With QIYAA_WRITE_DSP_VECTORS=1,
// initTestCase writes them from the inputs below; the *MatchSpec tests then check the app
// against the files, reading the inputs back from them as the Android tests do.

constexpr int kVectorSampleRates[] = {44'100, 48'000};
constexpr int kVectorFftSize = 1024;

template <typename Values>
QJsonArray NumberArray(const Values& values) {
    QJsonArray array;
    for (const auto value : values) {
        array.append(static_cast<double>(value));
    }
    return array;
}

std::vector<double> Numbers(const QJsonValue& value) {
    std::vector<double> numbers;
    for (const QJsonValue& item : value.toArray()) {
        numbers.push_back(item.toDouble());
    }
    return numbers;
}

QJsonObject SettingsJson(const Audio::EqSettings& settings) {
    return {
        {QStringLiteral("enabled"), settings.enabled},
        {QStringLiteral("preampDb"), settings.preampDb},
        {QStringLiteral("bandsDb"), NumberArray(settings.bandsDb)}
    };
}

Audio::EqSettings SettingsFrom(const QJsonObject& object) {
    Audio::EqSettings settings;
    settings.enabled = object.value(QStringLiteral("enabled")).toBool();
    settings.preampDb = object.value(QStringLiteral("preampDb")).toDouble();
    const std::vector<double> bands = Numbers(object.value(QStringLiteral("bandsDb")));
    std::copy_n(
        bands.begin(), std::min<size_t>(bands.size(), Audio::kEqBands), settings.bandsDb.begin()
    );
    return settings;
}

QJsonObject MakeEqResponse() {
    QList<std::pair<QString, Audio::EqSettings>> cases;
    cases << std::pair{QStringLiteral("flat"), Audio::EqSettings{}};
    for (const Audio::EqPreset& preset : Audio::BuiltinEqPresets()) {
        cases << std::pair{QStringLiteral("preset: ") + preset.name, preset.settings};
    }
    auto uniform = [](double db) {
        Audio::EqSettings settings;
        settings.bandsDb.fill(db);
        return settings;
    };
    Audio::EqSettings alternating, oneKhz, preampUp = uniform(0), preampDown = uniform(0);
    for (int band = 0; band < Audio::kEqBands; ++band) {
        alternating.bandsDb[band] = band % 2 == 0 ? Audio::kEqMaxDb : -Audio::kEqMaxDb;
    }
    oneKhz.bandsDb[4] = Audio::kEqMaxDb;
    preampUp.preampDb = 6;
    preampDown.preampDb = -12;
    Audio::EqSettings disabled = uniform(Audio::kEqMaxDb);
    disabled.enabled = false;
    cases << std::pair{QStringLiteral("all +12"), uniform(Audio::kEqMaxDb)}
          << std::pair{QStringLiteral("all -12"), uniform(-Audio::kEqMaxDb)}
          << std::pair{QStringLiteral("alternating +12/-12"), alternating}
          << std::pair{QStringLiteral("1 kHz +12"), oneKhz}
          << std::pair{QStringLiteral("preamp +6"), preampUp}
          << std::pair{QStringLiteral("preamp -12"), preampDown}
          << std::pair{QStringLiteral("disabled, bands +12"), disabled};

    std::vector<double> frequencies(Audio::kEqBandHz.begin(), Audio::kEqBandHz.end());
    for (int i = 0; i <= 30; ++i) {
        frequencies.push_back(std::round(20.0 * std::pow(1000.0, i / 30.0) * 100) / 100);
    }
    std::sort(frequencies.begin(), frequencies.end());
    frequencies.erase(std::unique(frequencies.begin(), frequencies.end()), frequencies.end());

    QJsonArray caseArray;
    for (const auto& [name, settings] : cases) {
        for (const int sampleRate : kVectorSampleRates) {
            QJsonArray response;
            for (const double hz : frequencies) {
                response.append(Audio::EqualizerDsp::ResponseDb(settings, hz, sampleRate));
            }
            caseArray.append(QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("sampleRate"), sampleRate},
                {QStringLiteral("settings"), SettingsJson(settings)},
                {QStringLiteral("responseDb"), response}
            });
        }
    }
    return {
        {QStringLiteral("bandsHz"), NumberArray(Audio::kEqBandHz)},
        {QStringLiteral("q"), 1.2},
        {QStringLiteral("tolerance"), QJsonObject{{QStringLiteral("db"), 1e-4}}},
        {QStringLiteral("frequenciesHz"), NumberArray(frequencies)},
        {QStringLiteral("cases"), caseArray}
    };
}

QJsonObject MakeEqPresets() {
    QJsonArray presets;
    for (const Audio::EqPreset& preset : Audio::BuiltinEqPresets()) {
        QJsonArray levels;
        for (const double db : preset.settings.bandsDb) {
            levels.append(Audio::DbToEqf(db));
        }
        presets.append(QJsonObject{
            {QStringLiteral("name"), preset.name},
            {QStringLiteral("levels"), levels},
            {QStringLiteral("preampLevel"), Audio::DbToEqf(preset.settings.preampDb)},
            {QStringLiteral("bandsDb"), NumberArray(preset.settings.bandsDb)},
            {QStringLiteral("preampDb"), preset.settings.preampDb}
        });
    }
    return {
        {QStringLiteral("tolerance"), QJsonObject{{QStringLiteral("db"), 1e-9}}},
        {QStringLiteral("presets"), presets}
    };
}

QByteArray EqfWithFirstBandByte(char byte) {
    QByteArray file = Audio::WriteEqf({Audio::EqPreset{QStringLiteral("p"), {}}});
    file[31 + 257] = byte;
    return file;
}

QJsonObject MakeEqf() {
    QJsonArray levelToDb;
    for (int level = 0; level <= 65; ++level) {
        levelToDb.append(QJsonObject{
            {QStringLiteral("level"), level}, {QStringLiteral("db"), Audio::EqfToDb(level)}
        });
    }
    QJsonArray dbToLevel;
    for (const double db :
         {-13.0, -12.0, -6.0, -0.19, -0.1, 0.0, 0.1, 0.19, 0.2, 5.904761904761905, 6.0, 11.9, 12.0,
          13.0}) {
        const int level = Audio::DbToEqf(db);
        dbToLevel.append(QJsonObject{
            {QStringLiteral("db"), db},
            {QStringLiteral("level"), level},
            {QStringLiteral("byte"), 64 - level}
        });
    }
    QJsonArray parsedBytes;
    for (const int byte : {0, 1, 31, 32, 33, 63, 64, 100, 255}) {
        parsedBytes.append(QJsonObject{
            {QStringLiteral("byte"), byte},
            {QStringLiteral("db"),
             Audio::ParseEqf(EqfWithFirstBandByte(static_cast<char>(byte)))
                 .first()
                 .settings.bandsDb[0]}
        });
    }
    return {
        {QStringLiteral("header"), QStringLiteral("Winamp EQ library file v1.1\u001a!--")},
        {QStringLiteral("nameBytes"), 257},
        {QStringLiteral("recordBytes"), 268},
        {QStringLiteral("tolerance"), QJsonObject{{QStringLiteral("db"), 1e-9}}},
        {QStringLiteral("levelToDb"), levelToDb},
        {QStringLiteral("dbToLevel"), dbToLevel},
        {QStringLiteral("parsedBytes"), parsedBytes}
    };
}

std::vector<float> MonoSine(double hz, double amplitude, int sampleRate, int count) {
    std::vector<float> samples(count);
    for (int i = 0; i < count; ++i) {
        samples[i] =
            static_cast<float>(amplitude * std::sin(2 * std::numbers::pi * hz * i / sampleRate));
    }
    return samples;
}

std::array<float, Vis::kSpectrumBars> SineLevels(double hz, double amplitude, int sampleRate) {
    Vis::Analyzer analyzer(kVectorFftSize);
    const std::vector<float>& spectrum =
        analyzer.analyze(MonoSine(hz, amplitude, sampleRate, kVectorFftSize));
    return Vis::SpectrumLevels(spectrum, sampleRate, kVectorFftSize);
}

QJsonObject MakeSpectrum() {
    QJsonObject bandsByRate;
    QJsonArray cases;
    for (const int sampleRate : kVectorSampleRates) {
        QJsonArray bands;
        for (const Vis::SpectrumBand& band : Vis::SpectrumBands(sampleRate, kVectorFftSize)) {
            bands.append(QJsonObject{
                {QStringLiteral("lowHz"), band.lowHz},
                {QStringLiteral("highHz"), band.highHz},
                {QStringLiteral("firstBin"), band.firstBin},
                {QStringLiteral("endBin"), band.endBin}
            });
        }
        bandsByRate.insert(QString::number(sampleRate), bands);
        QList<std::pair<double, double>> sines;
        for (const double hz :
             {60.0, 100.0, 250.0, 440.0, 1000.0, 2500.0, 5000.0, 10000.0, 15000.0}) {
            sines << std::pair{hz, 1.0};
        }
        sines << std::pair{1000.0, 0.1} << std::pair{1000.0, 0.01} << std::pair{0.0, 0.0};
        for (const auto& [hz, amplitude] : sines) {
            cases.append(QJsonObject{
                {QStringLiteral("sampleRate"), sampleRate},
                {QStringLiteral("sine"),
                 QJsonObject{{QStringLiteral("hz"), hz}, {QStringLiteral("amplitude"), amplitude}}},
                {QStringLiteral("levels"), NumberArray(SineLevels(hz, amplitude, sampleRate))}
            });
        }
    }
    return {
        {QStringLiteral("fftSize"), kVectorFftSize},
        {QStringLiteral("tolerance"), QJsonObject{{QStringLiteral("level"), 1e-3}}},
        {QStringLiteral("bands"), bandsByRate},
        {QStringLiteral("cases"), cases}
    };
}

// The first element where actual and expected differ by more than the tolerance, as a message.
QString Mismatch(
    const QString& where,
    const std::vector<double>& actual,
    const std::vector<double>& expected,
    double tolerance
) {
    if (actual.size() != expected.size()) {
        return QStringLiteral("%1: %2 values, expected %3")
            .arg(where)
            .arg(actual.size())
            .arg(expected.size());
    }
    for (size_t i = 0; i < actual.size(); ++i) {
        if (!(std::abs(actual[i] - expected[i]) <= tolerance)) {
            return QStringLiteral("%1 [%2]: %3, expected %4 ± %5")
                .arg(where)
                .arg(i)
                .arg(actual[i], 0, 'g', 17)
                .arg(expected[i], 0, 'g', 17)
                .arg(tolerance);
        }
    }
    return {};
}
}  // namespace

class TestDsp : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() {
        if (qEnvironmentVariableIsSet("QIYAA_WRITE_DSP_VECTORS")) {
            Tests::WriteSpecObject(QStringLiteral("dsp/eq-response.json"), MakeEqResponse());
            Tests::WriteSpecObject(QStringLiteral("dsp/eq-presets.json"), MakeEqPresets());
            Tests::WriteSpecObject(QStringLiteral("dsp/eqf.json"), MakeEqf());
            Tests::WriteSpecObject(QStringLiteral("dsp/spectrum.json"), MakeSpectrum());
        }
    }

    void eqResponseMatchesSpec() {
        const QJsonObject spec = Tests::SpecObject(QStringLiteral("dsp/eq-response.json"));
        const double tolerance = spec["tolerance"]["db"].toDouble();
        const std::vector<double> bands = Numbers(spec["bandsHz"]);
        QVERIFY(
            std::equal(bands.begin(), bands.end(), Audio::kEqBandHz.begin(), Audio::kEqBandHz.end())
        );
        const std::vector<double> frequencies = Numbers(spec["frequenciesHz"]);
        const QJsonArray cases = spec["cases"].toArray();
        QVERIFY(cases.size() >= 2 * 25);
        for (const QJsonValue& value : cases) {
            const QJsonObject item = value.toObject();
            const Audio::EqSettings settings = SettingsFrom(item["settings"].toObject());
            const int sampleRate = item["sampleRate"].toInt();
            std::vector<double> actual;
            for (const double hz : frequencies) {
                actual.push_back(Audio::EqualizerDsp::ResponseDb(settings, hz, sampleRate));
            }
            const QString problem = Mismatch(
                QStringLiteral("%1 @ %2").arg(item["name"].toString()).arg(sampleRate), actual,
                Numbers(item["responseDb"]), tolerance
            );
            QVERIFY2(problem.isEmpty(), qPrintable(problem));
        }
    }

    void eqPresetsMatchSpec() {
        const QJsonObject spec = Tests::SpecObject(QStringLiteral("dsp/eq-presets.json"));
        const double tolerance = spec["tolerance"]["db"].toDouble();
        const QJsonArray expected = spec["presets"].toArray();
        const QList<Audio::EqPreset> presets = Audio::BuiltinEqPresets();
        QCOMPARE(presets.size(), expected.size());
        for (int i = 0; i < presets.size(); ++i) {
            const QJsonObject item = expected[i].toObject();
            QCOMPARE(presets[i].name, item["name"].toString());
            const std::vector<double> actual(
                presets[i].settings.bandsDb.begin(), presets[i].settings.bandsDb.end()
            );
            const QString problem =
                Mismatch(presets[i].name, actual, Numbers(item["bandsDb"]), tolerance);
            QVERIFY2(problem.isEmpty(), qPrintable(problem));
            const std::vector<double> levels = Numbers(item["levels"]);
            for (int band = 0; band < Audio::kEqBands; ++band) {
                QCOMPARE(
                    Audio::EqfToDb(static_cast<int>(levels[band])), item["bandsDb"][band].toDouble()
                );
            }
            QCOMPARE(Audio::EqfToDb(item["preampLevel"].toInt()), presets[i].settings.preampDb);
        }
    }

    void eqfMatchesSpec() {
        const QJsonObject spec = Tests::SpecObject(QStringLiteral("dsp/eqf.json"));
        const double tolerance = spec["tolerance"]["db"].toDouble();
        const QByteArray file = Audio::WriteEqf({Audio::EqPreset{QStringLiteral("p"), {}}});
        const QByteArray header = spec["header"].toString().toLatin1();
        QVERIFY(file.startsWith(header));
        QCOMPARE(file.size(), header.size() + spec["recordBytes"].toInt());
        QCOMPARE(spec["recordBytes"].toInt(), spec["nameBytes"].toInt() + Audio::kEqBands + 1);
        for (const QJsonValue& value : spec["levelToDb"].toArray()) {
            const int level = value["level"].toInt();
            QVERIFY2(
                std::abs(Audio::EqfToDb(level) - value["db"].toDouble()) <= tolerance,
                qPrintable(QStringLiteral("level %1").arg(level))
            );
        }
        for (const QJsonValue& value : spec["dbToLevel"].toArray()) {
            const double db = value["db"].toDouble();
            QCOMPARE(Audio::DbToEqf(db), value["level"].toInt());
            QCOMPARE(64 - Audio::DbToEqf(db), value["byte"].toInt());
        }
        for (const QJsonValue& value : spec["parsedBytes"].toArray()) {
            const int byte = value["byte"].toInt();
            const double db = Audio::ParseEqf(EqfWithFirstBandByte(static_cast<char>(byte)))
                                  .first()
                                  .settings.bandsDb[0];
            QVERIFY2(
                std::abs(db - value["db"].toDouble()) <= tolerance,
                qPrintable(QStringLiteral("byte %1: %2").arg(byte).arg(db))
            );
        }
    }

    void spectrumMatchesSpec() {
        const QJsonObject spec = Tests::SpecObject(QStringLiteral("dsp/spectrum.json"));
        const double tolerance = spec["tolerance"]["level"].toDouble();
        QCOMPARE(spec["fftSize"].toInt(), kVectorFftSize);
        const QJsonObject bandsByRate = spec["bands"].toObject();
        QCOMPARE(bandsByRate.size(), static_cast<int>(std::size(kVectorSampleRates)));
        for (auto it = bandsByRate.constBegin(); it != bandsByRate.constEnd(); ++it) {
            const auto bands = Vis::SpectrumBands(it.key().toInt(), kVectorFftSize);
            const QJsonArray expected = it.value().toArray();
            QCOMPARE(expected.size(), Vis::kSpectrumBars);
            for (int bar = 0; bar < Vis::kSpectrumBars; ++bar) {
                QCOMPARE(bands[bar].firstBin, expected[bar]["firstBin"].toInt());
                QCOMPARE(bands[bar].endBin, expected[bar]["endBin"].toInt());
                QVERIFY(std::abs(bands[bar].lowHz - expected[bar]["lowHz"].toDouble()) < 1e-9);
                QVERIFY(std::abs(bands[bar].highHz - expected[bar]["highHz"].toDouble()) < 1e-9);
            }
        }
        for (const QJsonValue& value : spec["cases"].toArray()) {
            const QJsonObject sine = value["sine"].toObject();
            const int sampleRate = value["sampleRate"].toInt();
            const auto levels =
                SineLevels(sine["hz"].toDouble(), sine["amplitude"].toDouble(), sampleRate);
            const QString problem = Mismatch(
                QStringLiteral("%1 Hz × %2 @ %3")
                    .arg(sine["hz"].toDouble())
                    .arg(sine["amplitude"].toDouble())
                    .arg(sampleRate),
                std::vector<double>(levels.begin(), levels.end()), Numbers(value["levels"]),
                tolerance
            );
            QVERIFY2(problem.isEmpty(), qPrintable(problem));
        }
    }

    void flatIsTransparent() {
        Audio::EqSettings settings;
        for (double hz : {30.0, 60.0, 1000.0, 10'000.0, 18'000.0}) {
            QVERIFY(std::abs(Audio::EqualizerDsp::ResponseDb(settings, hz, 44'100)) < 0.01);
        }
    }

    void bandBoostHitsItsFrequency() {
        Audio::EqSettings settings;
        settings.bandsDb[4] = 12;  // 1 kHz
        QVERIFY(std::abs(Audio::EqualizerDsp::ResponseDb(settings, 1000, 44'100) - 12) < 0.1);
        QVERIFY(std::abs(Audio::EqualizerDsp::ResponseDb(settings, 60, 44'100)) < 0.5);
        QVERIFY(std::abs(Audio::EqualizerDsp::ResponseDb(settings, 16'000, 44'100)) < 0.5);
    }

    void preampShiftsTheResponseAndDisablingMakesItFlat() {
        Audio::EqSettings settings;
        settings.preampDb = -6;
        QVERIFY(std::abs(Audio::EqualizerDsp::ResponseDb(settings, 1000, 48'000) + 6) < 0.01);
        settings.enabled = false;
        QCOMPARE(Audio::EqualizerDsp::ResponseDb(settings, 1000, 48'000), 0.0);
    }

    void processingMatchesResponse() {
        Audio::EqualizerDsp lowEq;
        lowEq.setSampleRate(44'100);
        Audio::EqSettings settings;
        settings.bandsDb[0] = -12;  // 60 Hz cut
        lowEq.publish(settings);
        auto lowTone = StereoSine(60, 44'100, 44'100);
        auto midTone = StereoSine(3000, 44'100, 44'100);
        const double lowInputRms = Rms(lowTone, 4410), midInputRms = Rms(midTone, 4410);
        lowEq.process(lowTone);
        Audio::EqualizerDsp midEq;
        midEq.setSampleRate(44'100);
        midEq.publish(settings);
        midEq.process(midTone);
        const double lowDb = 20 * std::log10(Rms(lowTone, 4410) / lowInputRms);
        const double midDb = 20 * std::log10(Rms(midTone, 4410) / midInputRms);
        QVERIFY2(std::abs(lowDb + 12) < 0.5, qPrintable(QString::number(lowDb)));
        QVERIFY2(std::abs(midDb) < 0.3, qPrintable(QString::number(midDb)));
    }

    void settingsChangingWhileProcessingKeepsOutputBounded() {
        Audio::EqualizerDsp eq;
        eq.setSampleRate(48'000);
        auto samples = StereoSine(440, 48'000, 48'000);
        for (int block = 0; block < 100; ++block) {
            Audio::EqSettings settings;
            settings.bandsDb[block % 10] = (block % 2 ? 12 : -12);
            eq.publish(settings);
            eq.process(std::span(samples).subspan(block * 480 * 2, 480 * 2));
        }
        for (float sample : samples) {
            QVERIFY(std::isfinite(sample) && std::abs(sample) < 4.0f);
        }
    }

    void presetsLookRight() {
        const auto presets = Audio::BuiltinEqPresets();
        QCOMPARE(presets.size(), 17);
        const auto bass =
            std::find_if(presets.cbegin(), presets.cend(), [](const Audio::EqPreset& preset) {
                return preset.name == "Full Bass";
            });
        QVERIFY(bass != presets.cend());
        QVERIFY(bass->settings.bandsDb[0] > 5);
        QVERIFY(bass->settings.bandsDb[9] < -5);
        QCOMPARE(Audio::EqfToDb(1), -12.0);
        QCOMPARE(Audio::EqfToDb(64), 12.0);
    }

    void eqfRoundTripKeepsNamesAndLevels() {
        Audio::EqPreset preset;
        preset.name = QStringLiteral("My EQ");
        for (int i = 0; i < Audio::kEqBands; ++i) {
            preset.settings.bandsDb[i] = -12.0 + i * 2.6;
        }
        preset.settings.preampDb = 3.0;
        const QByteArray file = Audio::WriteEqf({preset, Audio::BuiltinEqPresets().first()});
        QCOMPARE(file.size(), 31 + 2 * 268);  // header + 2 presets (name 257 + 11 values)
        QVERIFY(file.startsWith("Winamp EQ library file v1.1\x1a!--"));
        const QList<Audio::EqPreset> readBack = Audio::ParseEqf(file);
        QCOMPARE(readBack.size(), 2);
        QCOMPARE(readBack[0].name, QStringLiteral("My EQ"));
        for (int i = 0; i < Audio::kEqBands; ++i) {
            // 64 levels over 24 dB: half a step (0.19 dB) + rounding to 0.1 dB.
            QVERIFY2(
                std::abs(readBack[0].settings.bandsDb[i] - preset.settings.bandsDb[i]) <= 0.25,
                qPrintable(QString::number(i))
            );
        }
        QVERIFY(std::abs(readBack[0].settings.preampDb - 3.0) <= 0.25);
        QCOMPARE(readBack[1].name, QStringLiteral("Classical"));
    }

    void eqfStoresMaxAsZeroAndMinAsSixtyThree() {
        // Max +12 dB is stored as 0, min -12 dB as 63 (webamp's max/min sample files).
        QByteArray file("Winamp EQ library file v1.1\x1a!--");
        QByteArray name("max");
        name.append(QByteArray(257 - name.size(), '\0'));
        file += name + QByteArray(10, static_cast<char>(0)) + QByteArray(1, static_cast<char>(63));
        const QList<Audio::EqPreset> presets = Audio::ParseEqf(file);
        QCOMPARE(presets[0].settings.bandsDb[0], 12.0);
        QCOMPARE(presets[0].settings.preampDb, -12.0);
    }

    void eqfParserRejectsWhatIsNotAPreset() {
        QVERIFY_THROWS_EXCEPTION(Audio::Error, Audio::ParseEqf("not an eqf"));
        // The header alone: a library without presets.
        QVERIFY_THROWS_EXCEPTION(
            Audio::Error, Audio::ParseEqf(QByteArray("Winamp EQ library file v1.1\x1a!--", 31))
        );
        try {
            Audio::ParseEqf("not an eqf");
        } catch (const Audio::Error& error) {
            QVERIFY2(QByteArray(error.what()).contains("10 bytes"), error.what());
        }
    }

    void graphSplinePassesThroughBands() {
        Audio::EqSettings settings;
        settings.bandsDb[3] = 12;
        const QList<double> curve = Ui::EqualizerWindow::GraphCurve(settings);
        QCOMPARE(curve.size(), 9 * 12 + 1);
        QVERIFY(std::abs(curve[3 * 12] - 0) < 1e-6);  // +12 dB = top row
        QVERIFY(std::abs(curve[0] - 9) < 1e-6);  // 0 dB = middle
        QVERIFY(curve[30] < 9 && curve[42] < 9);
    }

    void analyzerFindsTheTone() {
        Vis::Analyzer analyzer(1024);
        const double sampleRate = 44'100, hz = 1000;
        std::vector<float> mono(1024);
        for (int i = 0; i < 1024; ++i) {
            mono[i] = static_cast<float>(std::sin(2 * std::numbers::pi * hz * i / sampleRate));
        }
        const auto& spectrumDb = analyzer.analyze(mono);
        QCOMPARE(static_cast<int>(spectrumDb.size()), 513);
        const int peak = static_cast<int>(
            std::max_element(spectrumDb.begin(), spectrumDb.end()) - spectrumDb.begin()
        );
        QCOMPARE(peak, static_cast<int>(std::lround(hz / (sampleRate / 1024))));
        QVERIFY(std::abs(spectrumDb[peak]) < 1.5);  // full-scale sine ~ 0 dBFS
        QVERIFY(spectrumDb[400] < -50);
    }

    void spectrumAndScopeRender() {
        const Skins::Skin skin = Skins::Skin::BuiltinBase();
        Vis::Analyzer analyzer(1024);
        std::vector<float> left(1024), right(1024), mono(1024);
        for (int i = 0; i < 1024; ++i) {
            left[i] = right[i] = mono[i] =
                0.8f * static_cast<float>(std::sin(2 * std::numbers::pi * 200 * i / 44'100.0));
        }
        const auto& spectrumDb = analyzer.analyze(mono);
        Vis::VisFrame frame{left, right, spectrumDb, 44'100, 1024};

        for (auto factory : {Vis::MakeSpectrum, Vis::MakeOscilloscope}) {
            auto visualizer = factory();
            QImage image(76, 16, QImage::Format_ARGB32);
            image.fill(Qt::black);
            visualizer->update(frame);
            QPainter painter(&image);
            visualizer->render(painter, QRect(0, 0, 76, 16), skin);
            painter.end();
            int litPixels = 0;
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 76; ++x) {
                    litPixels += image.pixel(x, y) != qRgb(0, 0, 0);
                }
            }
            QVERIFY2(litPixels > 10, qPrintable(visualizer->name()));
        }
    }

    void eqfReadsCentreAsZeroAndClampsOutOfRangeBytes() {
        QByteArray file("Winamp EQ library file v1.1\x1a!--", 31);
        QByteArray name("Flat");
        name.append(QByteArray(257 - name.size(), '\0'));
        file += name;
        file += QByteArray(10, static_cast<char>(31));  // Winamp's own midline.EQF: 64 - 31 = 33
        file += static_cast<char>(255);  // garbage: must not reach the DSP as -85 dB
        const QList<Audio::EqPreset> presets = Audio::ParseEqf(file);
        for (int band = 0; band < Audio::kEqBands; ++band) {
            QCOMPARE(presets[0].settings.bandsDb[band], 0.0);
        }
        QCOMPARE(presets[0].settings.preampDb, -12.0);
        QCOMPARE(Audio::WriteEqf(presets).mid(31 + 257, 10), QByteArray(10, static_cast<char>(31)));
    }

    void eqfKeepsNonLatinNames() {
        Audio::EqPreset preset;
        preset.name = QStringLiteral("Мой пресет");
        const QList<Audio::EqPreset> presets = Audio::ParseEqf(Audio::WriteEqf({preset}));
        QCOMPARE(presets[0].name, preset.name);
    }
};

QTEST_MAIN(TestDsp)
#include "dsp_test.moc"
