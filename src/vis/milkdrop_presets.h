#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace Vis {

enum class PresetTransition { Cut, Blend };

class MilkdropPresets {
public:
    struct Preset {
        QString name;
        QString path;
        bool builtIn = true;
    };

    void load(const QString& builtInDirectory, const QString& userDirectory);

    int size() const { return static_cast<int>(presetList.size()); }
    bool isEmpty() const { return presetList.isEmpty(); }
    const Preset& at(int index) const { return presetList.at(index); }
    int indexOf(const QString& name) const;
    QByteArray data(int index) const;

    int next(int current) const;
    int previous(int current) const;
    int random(int current) const;

private:
    QList<Preset> presetList;
};

}  // namespace Vis
