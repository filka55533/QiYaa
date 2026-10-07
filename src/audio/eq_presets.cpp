#include "audio/eq_presets.h"

#include "audio/error.h"

#include <QStringConverter>
#include <QStringDecoder>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace Audio {

namespace {
struct WinampPreset {
    const char* name;
    int preamp;
    std::array<int, kEqBands> bands;
};

// Port of webamp's presets/builtin.json (MIT), in the .eqf scale 1..64.
constexpr WinampPreset kWinampPresets[] = {
    {"Classical", 33, {33, 33, 33, 33, 33, 33, 20, 20, 20, 16}},
    {"Club", 33, {33, 33, 38, 42, 42, 42, 38, 33, 33, 33}},
    {"Dance", 33, {48, 44, 36, 32, 32, 22, 20, 20, 32, 32}},
    {"Laptop speakers/headphones", 33, {40, 50, 41, 26, 28, 35, 40, 48, 53, 56}},
    {"Large hall", 33, {49, 49, 42, 42, 33, 24, 24, 24, 33, 33}},
    {"Party", 33, {44, 44, 33, 33, 33, 33, 33, 33, 44, 44}},
    {"Pop", 33, {29, 40, 44, 45, 41, 30, 28, 28, 29, 29}},
    {"Reggae", 33, {33, 33, 31, 22, 33, 43, 43, 33, 33, 33}},
    {"Rock", 33, {45, 40, 23, 19, 26, 39, 47, 50, 50, 50}},
    {"Soft", 33, {40, 35, 30, 28, 30, 39, 46, 48, 50, 52}},
    {"Ska", 33, {28, 24, 25, 31, 39, 42, 47, 48, 50, 48}},
    {"Full Bass", 33, {48, 48, 48, 42, 35, 25, 18, 15, 14, 14}},
    {"Soft Rock", 33, {39, 39, 36, 31, 25, 23, 26, 31, 37, 47}},
    {"Full Treble", 33, {16, 16, 16, 25, 37, 50, 58, 58, 58, 60}},
    {"Full Bass & Treble", 33, {44, 42, 33, 20, 24, 35, 46, 50, 52, 52}},
    {"Live", 33, {24, 33, 39, 41, 42, 42, 39, 37, 37, 36}},
    {"Techno", 33, {45, 42, 33, 23, 24, 33, 45, 48, 48, 47}},
};

constexpr char kHeader[] = "Winamp EQ library file v1.1";
constexpr int kHeaderLength = sizeof(kHeader) - 1;
constexpr int kNameLength = 257;
constexpr int kValueCount = kEqBands + 1;

QString DecodeName(const QByteArray& raw) {
    QStringDecoder utf8Decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    const QString decoded = utf8Decoder.decode(raw);
    if (!utf8Decoder.hasError()) {
        return decoded;
    }
#ifdef Q_OS_WIN
    return QString::fromLocal8Bit(raw);
#else
    return QString::fromLatin1(raw);
#endif
}

QByteArray EncodeName(const QString& name) {
    QByteArray encoded;
    bool useUtf8 = true;
#ifdef Q_OS_WIN
    encoded = name.toLocal8Bit();
    useUtf8 = QString::fromLocal8Bit(encoded) != name;
#endif
    if (useUtf8) {
        encoded = name.toUtf8();
    }
    if (encoded.size() > kNameLength - 1) {
        encoded.truncate(kNameLength - 1);
        if (useUtf8) {  // don't cut a character in half
            while (!encoded.isEmpty() && (static_cast<quint8>(encoded.back()) & 0xC0) == 0x80) {
                encoded.chop(1);
            }
            if (!encoded.isEmpty() && static_cast<quint8>(encoded.back()) >= 0xC0) {
                encoded.chop(1);
            }
        }
    }
    return encoded;
}
}  // namespace

QList<EqPreset> ParseEqf(const QByteArray& data) {
    if (!data.startsWith(kHeader) || data.size() < kHeaderLength + 4) {
        throw Error(
            "not a Winamp EQ file: " + std::to_string(data.size())
            + " bytes that do not start with \"" + kHeader + "\""
        );
    }
    qsizetype offset = kHeaderLength + 4;  // skip ^Z "!--"
    QList<EqPreset> presets;
    while (offset + kNameLength + kValueCount <= data.size()) {
        const QByteArray rawName = data.mid(offset, kNameLength);
        const qsizetype nulIndex = rawName.indexOf('\0');
        EqPreset preset;
        preset.name = DecodeName(nulIndex >= 0 ? rawName.left(nulIndex) : rawName);
        offset += kNameLength;
        auto valueAt = [&](int index) {
            return 64 - static_cast<int>(static_cast<quint8>(data[offset + index]));
        };
        for (int band = 0; band < kEqBands; ++band) {
            preset.settings.bandsDb[band] = std::round(EqfToDb(valueAt(band)) * 10) / 10;
        }
        preset.settings.preampDb = std::round(EqfToDb(valueAt(kEqBands)) * 10) / 10;
        offset += kValueCount;
        presets << preset;
    }
    if (presets.isEmpty()) {
        throw Error(
            "Winamp EQ file of " + std::to_string(data.size())
            + " bytes holds no preset (one takes " + std::to_string(kNameLength + kValueCount)
            + " bytes after the " + std::to_string(kHeaderLength + 4) + "-byte header)"
        );
    }
    return presets;
}

QByteArray WriteEqf(const QList<EqPreset>& presets) {
    QByteArray data(kHeader);
    data += static_cast<char>(26);
    data += "!--";
    for (const EqPreset& preset : presets) {
        QByteArray name = EncodeName(preset.name);
        name.append(QByteArray(kNameLength - name.size(), '\0'));
        data += name;
        for (int band = 0; band < kEqBands; ++band) {
            data += static_cast<char>(64 - DbToEqf(preset.settings.bandsDb[band]));
        }
        data += static_cast<char>(64 - DbToEqf(preset.settings.preampDb));
    }
    return data;
}

// 33 is Winamp's centre notch (0 dB writes 33): exactly 0 dB, not the line's +0.19 dB.
double EqfToDb(int value) {
    value = std::clamp(value, 1, 64);
    return value == 33 ? 0.0 : (static_cast<double>(value) - 1.0) / 63.0 * 24.0 - 12.0;
}

int DbToEqf(double db) {
    return std::clamp(static_cast<int>(std::lround((db + 12.0) / 24.0 * 63.0 + 1.0)), 1, 64);
}

QList<EqPreset> BuiltinEqPresets() {
    QList<EqPreset> presets;
    for (const WinampPreset& source : kWinampPresets) {
        EqPreset preset;
        preset.name = QString::fromLatin1(source.name);
        preset.settings.preampDb = EqfToDb(source.preamp);
        for (int i = 0; i < kEqBands; ++i) {
            preset.settings.bandsDb[i] = EqfToDb(source.bands[i]);
        }
        presets << preset;
    }
    return presets;
}

}  // namespace Audio
