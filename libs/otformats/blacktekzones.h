#ifndef BLACKTEKZONES_H
#define BLACKTEKZONES_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

struct BlackTekZone
{
    struct Position3D { int x = 0; int y = 0; int z = 0; };
    struct CreatureEntry {
        QString category;
        QString name;
        int index = 0;
        Position3D position;
        bool hasPosition = false;
        quint32 intervalSeconds = 0;
        quint32 chance = 100;
        quint32 delayMilliseconds = 0;
        int wave = 0;
    };
    QString name;
    QString sourceFile;
    QString spawnType;
    QStringList flags;
    bool hasFlags = false;
    bool protection = false;
    QVector<Position3D> worldPositions;
    QVector<CreatureEntry> creatures;
    Position3D rangeStart;
    Position3D rangeEnd;
    bool hasRange = false;
    bool hasPositions = false;
    bool playerRelative = false;
    bool staged = false;
    int floor = 0;
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
};

struct BlackTekZoneLoadResult
{
    QVector<BlackTekZone> zones;
    QStringList errors;
    QStringList warnings;
    QStringList diagnostics() const { return errors + warnings; }
};

class BlackTekZones
{
public:
    static BlackTekZoneLoadResult parse(const QString &path, const QByteArray &bytes);
};

#endif
