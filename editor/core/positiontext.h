#ifndef POSITIONTEXT_H
#define POSITIONTEXT_H

#include <QRegularExpression>
#include <QString>
#include <QStringList>

// Position strings exchanged through the clipboard by the map context menu.
// Keep formatting and teleport clipboard parsing in sync.
namespace PositionText {

// Supported "Copy Position As" styles. Unknown styles fall back to plain.
inline QString formatText(const QString &format, int x, int y, int z)
{
    if (format == QLatin1String("tuple"))
        return QStringLiteral("(%1, %2, %3)").arg(x).arg(y).arg(z);
    if (format == QLatin1String("position"))
        return QStringLiteral("Position(%1, %2, %3)").arg(x).arg(y).arg(z);
    if (format == QLatin1String("lua"))
        return QStringLiteral("{x = %1, y = %2, z = %3}").arg(x).arg(y).arg(z);
    if (format == QLatin1String("json"))
        return QStringLiteral("{\"x\":%1,\"y\":%2,\"z\":%3}").arg(x).arg(y).arg(z);
    return QStringLiteral("%1, %2, %3").arg(x).arg(y).arg(z);
}

// Parse the five Copy Position As formats, including reordered keyed entries.
inline bool parseText(const QString &text, int *outX, int *outY, int *outZ)
{
    QString position = text.trimmed();
    if (position.startsWith(QLatin1String("Position("), Qt::CaseInsensitive))
        position.remove(0, 8);

    const bool keyed = position.startsWith(QLatin1Char('{'));
    if (keyed || position.startsWith(QLatin1Char('('))) {
        const QChar closing = QLatin1Char(keyed ? '}' : ')');
        if (!position.endsWith(closing)) return false;
        position = position.mid(1, position.size() - 2);
    }

    const auto parts = position.split(QLatin1Char(','));
    if (parts.size() != 3) return false;
    static const QRegularExpression named(QStringLiteral(
        R"coord(^\s*(?:"([xyz])"\s*:|([xyz])\s*=)\s*([0-9]+)\s*$)coord"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression unsignedInteger(QStringLiteral("^[0-9]+$"));

    int values[3] = {-1, -1, -1};
    for (int part = 0; part < 3; ++part) {
        int axis = part;
        QString digits = parts.at(part).trimmed();
        if (keyed) {
            const auto match = named.match(digits);
            if (!match.hasMatch()) return false;
            const QString key = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
            axis = key.at(0).toLower().unicode() - u'x';
            if (values[axis] >= 0) return false;
            digits = match.captured(3);
        } else if (!unsignedInteger.match(digits).hasMatch()) {
            return false;
        }

        bool converted = false;
        const int value = digits.toInt(&converted);
        if (!converted || value > (axis == 2 ? 15 : 65535)) return false;
        values[axis] = value;
    }

    if (outX) *outX = values[0];
    if (outY) *outY = values[1];
    if (outZ) *outZ = values[2];
    return true;
}

} // namespace PositionText

#endif
