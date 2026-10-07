#include "vis/milkdrop_presets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QRandomGenerator>

#include <algorithm>

namespace Vis {

namespace {

constexpr qint64 kMaxPresetBytes = 1024 * 1024;

QList<MilkdropPresets::Preset> ScanPresetDirectory(const QString& directory, bool builtIn) {
    QList<MilkdropPresets::Preset> presets;
    if (directory.isEmpty()) {
        return presets;
    }
    const QFileInfoList files =
        QDir(directory).entryInfoList({QStringLiteral("*.milk")}, QDir::Files | QDir::Readable);
    for (const QFileInfo& file : files) {
        presets.append({file.completeBaseName(), file.filePath(), builtIn});
    }
    std::sort(presets.begin(), presets.end(), [](const auto& first, const auto& second) {
        if (const int byName = QString::compare(first.name, second.name, Qt::CaseInsensitive)) {
            return byName < 0;
        }
        return first.path < second.path;
    });
    return presets;
}
}  // namespace

void MilkdropPresets::load(const QString& builtInDirectory, const QString& userDirectory) {
    presetList =
        ScanPresetDirectory(builtInDirectory, true) + ScanPresetDirectory(userDirectory, false);
}

int MilkdropPresets::indexOf(const QString& name) const {
    for (int i = 0; i < size(); ++i) {
        if (presetList[i].name == name) {
            return i;
        }
    }
    return -1;
}

QByteArray MilkdropPresets::data(int index) const {
    if (index < 0 || index >= size()) {
        return {};
    }
    QFile file(presetList[index].path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxPresetBytes) {
        return {};
    }
    return file.readAll();
}

int MilkdropPresets::next(int current) const {
    return isEmpty() ? -1 : (current + 1 + size()) % size();
}

int MilkdropPresets::previous(int current) const {
    return isEmpty() ? -1 : (current - 1 + size()) % size();
}

int MilkdropPresets::random(int current) const {
    if (isEmpty()) {
        return -1;
    }
    if (size() == 1) {
        return 0;
    }
    int index;
    do {
        index = static_cast<int>(QRandomGenerator::global()->bounded(size()));
    } while (index == current);
    return index;
}

}  // namespace Vis
