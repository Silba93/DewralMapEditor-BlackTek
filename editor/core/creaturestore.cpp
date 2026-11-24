#include "creaturestore.h"

#include "dmedatadir.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>

CreatureStore::CreatureStore(QObject *parent)
    : QAbstractListModel(parent)
{
}

int CreatureStore::monsterCount() const
{
    return static_cast<int>(std::count_if(
        m_creatures.cbegin(), m_creatures.cend(),
        [](const CreatureType &creature) { return !creature.isNpc; }));
}

int CreatureStore::npcCount() const
{
    return static_cast<int>(std::count_if(
        m_creatures.cbegin(), m_creatures.cend(),
        [](const CreatureType &creature) { return creature.isNpc; }));
}

bool CreatureStore::loadForVersion(int version)
{
    return loadForDir(QString::number(version));
}

bool CreatureStore::loadForDir(const QString &dirName)
{
    beginResetModel();
    m_creatures.clear();
    m_path = QDir(dmeDataDir())
                 .filePath(QStringLiteral("%1/creatures.xml").arg(dirName));
    const bool ok = QFile::exists(m_path) && loadFile(m_path);
    std::sort(m_creatures.begin(), m_creatures.end(),
              [](const CreatureType &a, const CreatureType &b) {
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });
    endResetModel();
    emit countChanged();
    return ok;
}

void CreatureStore::setErrorString(const QString &error)
{
    if (m_errorString == error) return;
    m_errorString = error;
    emit errorStringChanged();
}

bool CreatureStore::loadFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;

    QXmlStreamReader xml(&f);

    bool inNpcsSection = false;

    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        const auto tag = xml.name();
        if (token == QXmlStreamReader::StartElement) {
            if (tag == QLatin1String("npcs")) { inNpcsSection = true; continue; }
            if (tag == QLatin1String("monsters")) { inNpcsSection = false; continue; }

            const bool isCreature = tag == QLatin1String("creature")
                                    || tag == QLatin1String("monster")
                                    || tag == QLatin1String("npc");
            if (!isCreature) continue;

            const auto a = xml.attributes();
            CreatureType c;
            c.name = a.value(QLatin1String("name")).toString();
            if (c.name.isEmpty()) continue;
            c.isNpc = inNpcsSection || tag == QLatin1String("npc")
                      || a.value(QLatin1String("type")) == QLatin1String("npc");
            c.lookType = a.value(QLatin1String("looktype")).toInt();
            c.lookItem = a.value(QLatin1String("lookitem")).toInt();
            c.lookHead = a.value(QLatin1String("lookhead")).toInt();
            c.lookBody = a.value(QLatin1String("lookbody")).toInt();
            c.lookLegs = a.value(QLatin1String("looklegs")).toInt();
            c.lookFeet = a.value(QLatin1String("lookfeet")).toInt();
            m_creatures.push_back(std::move(c));
        } else if (token == QXmlStreamReader::EndElement) {
            if (tag == QLatin1String("npcs")) inNpcsSection = false;
        }
    }
    return !xml.hasError();
}

const CreatureStore::CreatureType *CreatureStore::byName(const QString &name) const
{
    for (const CreatureType &c : m_creatures)
        if (c.name.compare(name, Qt::CaseInsensitive) == 0) return &c;
    return nullptr;
}

const CreatureStore::CreatureType *CreatureStore::byNameAndType(
    const QString &name, bool isNpc) const
{
    for (const CreatureType &creature : m_creatures) {
        if (creature.isNpc == isNpc
            && creature.name.compare(name, Qt::CaseInsensitive) == 0) {
            return &creature;
        }
    }
    return nullptr;
}

int CreatureStore::rowForName(const QString &name) const
{
    for (int row = 0; row < m_creatures.size(); ++row)
        if (m_creatures[row].name.compare(name, Qt::CaseInsensitive) == 0)
            return row;
    return -1;
}

int CreatureStore::rowForCreature(const QString &name, bool isNpc) const
{
    for (int row = 0; row < m_creatures.size(); ++row) {
        if (m_creatures[row].isNpc == isNpc
            && m_creatures[row].name.compare(name, Qt::CaseInsensitive) == 0) {
            return row;
        }
    }
    return -1;
}

QVariantMap CreatureStore::creatureAt(int row) const
{
    QVariantMap result;
    if (row < 0 || row >= m_creatures.size()) return result;
    const CreatureType &c = m_creatures[row];
    result.insert(QStringLiteral("name"), c.name);
    result.insert(QStringLiteral("isNpc"), c.isNpc);
    result.insert(QStringLiteral("lookType"), c.lookType);
    result.insert(QStringLiteral("lookItem"), c.lookItem);
    result.insert(QStringLiteral("lookHead"), c.lookHead);
    result.insert(QStringLiteral("lookBody"), c.lookBody);
    result.insert(QStringLiteral("lookLegs"), c.lookLegs);
    result.insert(QStringLiteral("lookFeet"), c.lookFeet);
    return result;
}

bool CreatureStore::saveFile(const QVector<CreatureType> &creatures)
{
    if (m_path.isEmpty()) {
        setErrorString(QStringLiteral("No client profile is loaded."));
        return false;
    }
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        setErrorString(QStringLiteral("Cannot create the creature data directory."));
        return false;
    }

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        setErrorString(file.errorString());
        return false;
    }
    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("creatures"));
    for (const CreatureType &c : creatures) {
        xml.writeStartElement(QStringLiteral("creature"));
        xml.writeAttribute(QStringLiteral("name"), c.name);
        xml.writeAttribute(QStringLiteral("type"),
                           c.isNpc ? QStringLiteral("npc")
                                   : QStringLiteral("monster"));
        xml.writeAttribute(QStringLiteral("looktype"), QString::number(c.lookType));
        xml.writeAttribute(QStringLiteral("lookitem"), QString::number(c.lookItem));
        xml.writeAttribute(QStringLiteral("lookhead"), QString::number(c.lookHead));
        xml.writeAttribute(QStringLiteral("lookbody"), QString::number(c.lookBody));
        xml.writeAttribute(QStringLiteral("looklegs"), QString::number(c.lookLegs));
        xml.writeAttribute(QStringLiteral("lookfeet"), QString::number(c.lookFeet));
        xml.writeEndElement();
    }
    xml.writeEndElement();
    xml.writeEndDocument();
    if (xml.hasError() || !file.commit()) {
        setErrorString(file.errorString().isEmpty()
                           ? QStringLiteral("Could not save creatures.xml.")
                           : file.errorString());
        return false;
    }
    setErrorString(QString());
    return true;
}

bool CreatureStore::saveCreature(const QString &originalName,
                                 const QString &name, bool isNpc,
                                 int lookType, int lookItem,
                                 int lookHead, int lookBody,
                                 int lookLegs, int lookFeet)
{
    const QString cleanName = name.trimmed();
    if (cleanName.isEmpty() || lookType < 0 || lookItem < 0) return false;
    const int existing = rowForName(cleanName);
    const int original = rowForName(originalName);
    if (existing >= 0 && existing != original) {
        setErrorString(QStringLiteral("A creature named \"%1\" already exists.")
                           .arg(cleanName));
        return false;
    }

    const QVector<CreatureType> previous = m_creatures;
    beginResetModel();
    CreatureType value;
    value.name = cleanName;
    value.isNpc = isNpc;
    value.lookType = std::clamp(lookType, 0, 65535);
    value.lookItem = std::clamp(lookItem, 0, 65535);
    value.lookHead = std::clamp(lookHead, 0, 255);
    value.lookBody = std::clamp(lookBody, 0, 255);
    value.lookLegs = std::clamp(lookLegs, 0, 255);
    value.lookFeet = std::clamp(lookFeet, 0, 255);
    if (original >= 0) m_creatures[original] = value;
    else m_creatures.append(value);
    std::sort(m_creatures.begin(), m_creatures.end(),
              [](const CreatureType &a, const CreatureType &b) {
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });
    endResetModel();
    const bool ok = saveFile(m_creatures);
    if (!ok) {
        beginResetModel();
        m_creatures = previous;
        endResetModel();
    }
    emit countChanged();
    return ok;
}

bool CreatureStore::removeCreature(const QString &name)
{
    const int row = rowForName(name);
    if (row < 0) return false;
    const QVector<CreatureType> previous = m_creatures;
    beginResetModel();
    m_creatures.removeAt(row);
    endResetModel();
    const bool ok = saveFile(m_creatures);
    if (!ok) {
        beginResetModel();
        m_creatures = previous;
        endResetModel();
    }
    emit countChanged();
    return ok;
}

bool CreatureStore::removeAllMonsters()
{
    if (monsterCount() == 0) {
        setErrorString(QString());
        return true;
    }

    QVector<CreatureType> npcs = m_creatures;
    npcs.erase(std::remove_if(npcs.begin(), npcs.end(), [](const CreatureType &creature) {
                   return !creature.isNpc;
               }),
               npcs.end());
    if (!saveFile(npcs))
        return false;

    beginResetModel();
    m_creatures.swap(npcs);
    endResetModel();
    emit countChanged();
    return true;
}

bool CreatureStore::openMonstersFolder()
{
    if (m_path.isEmpty()) {
        setErrorString(QStringLiteral("No client profile is loaded."));
        return false;
    }
    const QString folder = QFileInfo(m_path).absolutePath();
    if (!QDir().mkpath(folder) || !QDesktopServices::openUrl(QUrl::fromLocalFile(folder))) {
        setErrorString(QStringLiteral("Could not open the monster catalog folder."));
        return false;
    }
    setErrorString(QString());
    return true;
}

namespace {
QString maskLuaSource(const QString &source, QStringList &stringLiterals)
{
    QString masked;
    masked.reserve(source.size());
    for (qsizetype i = 0; i < source.size();) {
        const QChar ch = source[i];
        const bool comment = ch == QLatin1Char('-') && i + 1 < source.size()
                          && source[i + 1] == QLatin1Char('-');
        const qsizetype bracket = i + (comment ? 2 : 0);
        if (bracket < source.size() && source[bracket] == QLatin1Char('[')) {
            qsizetype open = bracket + 1;
            while (open < source.size() && source[open] == QLatin1Char('='))
                ++open;
            if (open < source.size() && source[open] == QLatin1Char('[')) {
                const QString close = QLatin1Char(']')
                                    + QString(open - bracket - 1, QLatin1Char('='))
                                    + QLatin1Char(']');
                const qsizetype end = source.indexOf(close, open + 1);
                if (comment) {
                    masked.append(QLatin1Char(' '));
                } else {
                    const qsizetype literalIndex = stringLiterals.size();
                    stringLiterals.append(end < 0 ? QString() : source.mid(open + 1, end - open - 1));
                    masked.append(QStringLiteral("__DME_STRING_%1__").arg(literalIndex));
                }
                i = end < 0 ? source.size() : end + close.size();
                continue;
            }
        }
        if (comment) {
            while (i < source.size() && source[i] != QLatin1Char('\n'))
                ++i;
            masked.append(QLatin1Char(' '));
            continue;
        }
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            const QChar quote = ch;
            QString value;
            ++i;
            while (i < source.size() && source[i] != quote) {
                if (source[i] == QLatin1Char('\\') && i + 1 < source.size()) {
                    const QChar escaped = source[i + 1];
                    if (escaped == quote || escaped == QLatin1Char('\\')) value.append(escaped);
                    else if (escaped == QLatin1Char('n')) value.append(QLatin1Char('\n'));
                    else if (escaped == QLatin1Char('t')) value.append(QLatin1Char('\t'));
                    else value.append(QLatin1Char('\\')).append(escaped);
                    i += 2;
                } else {
                    value.append(source[i++]);
                }
            }
            if (i < source.size()) ++i;
            const qsizetype literalIndex = stringLiterals.size();
            stringLiterals.append(std::move(value));
            masked.append(QStringLiteral("__DME_STRING_%1__").arg(literalIndex));
            continue;
        }
        masked.append(ch);
        ++i;
    }
    return masked;
}

bool readLuaCreatureDefinition(const QString &path,
                               CreatureStore::CreatureType &creature,
                               QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = file.errorString();
        return false;
    }
    QStringList stringLiterals;
    const QString text = maskLuaSource(QString::fromUtf8(file.readAll()), stringLiterals);
    static const QRegularExpression constructor(
        QStringLiteral("Game\\s*\\.\\s*createMonsterType\\s*\\(\\s*__DME_STRING_(\\d+)__\\s*\\)"));
    const auto registeredMatch = constructor.match(text);
    if (!registeredMatch.hasMatch()) {
        static const QRegularExpression constructorCall(
            QStringLiteral("Game\\s*\\.\\s*createMonsterType\\s*\\("));
        if (constructorCall.match(text).hasMatch())
            error = QStringLiteral("Monster type name must be a string literal.");
        return false;
    }
    const int registeredIndex = registeredMatch.captured(1).toInt();
    if (registeredIndex < 0 || registeredIndex >= stringLiterals.size()) {
        error = QStringLiteral("Monster type name is invalid.");
        return false;
    }
    creature.name = stringLiterals.at(registeredIndex).trimmed();
    static const QRegularExpression displayName(
        QStringLiteral("monster\\s*\\.\\s*name\\s*=\\s*__DME_STRING_(\\d+)__"));
    const auto displayMatch = displayName.match(text);
    if (displayMatch.hasMatch()) {
        const int displayIndex = displayMatch.captured(1).toInt();
        if (displayIndex >= 0 && displayIndex < stringLiterals.size()) {
            const QString display = stringLiterals.at(displayIndex).trimmed();
            if (creature.name.compare(display, Qt::CaseInsensitive) == 0)
                creature.name = display;
        }
    }
    if (creature.name.isEmpty()) {
        error = QStringLiteral("Monster type name is empty.");
        return false;
    }

    const int outfitStart = text.indexOf(
        QRegularExpression(QStringLiteral("monster\\s*\\.\\s*outfit\\s*=\\s*\\{")));
    if (outfitStart >= 0) {
        const int openBrace = text.indexOf(QLatin1Char('{'), outfitStart);
        const int closeBrace = text.indexOf(QLatin1Char('}'), openBrace + 1);
        if (closeBrace >= 0) {
            const QString outfit = text.mid(openBrace + 1, closeBrace - openBrace - 1);
            const auto readNumber = [&outfit](const QString &field) {
                const QRegularExpression expression(
                    QStringLiteral("(?:^|[,\\s])%1\\s*=\\s*(\\d+)(?=\\s*(?:,|$))").arg(field));
                const auto match = expression.match(outfit);
                return match.hasMatch() ? match.captured(1).toInt() : 0;
            };
            creature.lookType = readNumber(QStringLiteral("lookType"));
            creature.lookItem = readNumber(QStringLiteral("lookTypeEx"));
            creature.lookHead = readNumber(QStringLiteral("lookHead"));
            creature.lookBody = readNumber(QStringLiteral("lookBody"));
            creature.lookLegs = readNumber(QStringLiteral("lookLegs"));
            creature.lookFeet = readNumber(QStringLiteral("lookFeet"));
        }
    }
    return true;
}

bool readCreatureDefinition(const QString &path,
                            CreatureStore::CreatureType &creature,
                            QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QXmlStreamReader xml(&file);
    if (!xml.readNextStartElement()) {
        if (error) *error = QStringLiteral("Empty XML file.");
        return false;
    }
    const QString root = xml.name().toString().toLower();
    if (root != QLatin1String("monster") && root != QLatin1String("npc")
        && root != QLatin1String("creature")) {
        if (error) *error = QStringLiteral("Not a monster or NPC definition.");
        return false;
    }
    const auto rootAttributes = xml.attributes();
    creature.name = rootAttributes.value(QLatin1String("name")).toString();
    creature.isNpc = root == QLatin1String("npc")
                     || rootAttributes.value(QLatin1String("type"))
                            .toString().compare(QLatin1String("npc"),
                                                Qt::CaseInsensitive) == 0;
    while (xml.readNextStartElement()) {
        const QString tag = xml.name().toString().toLower();
        if (tag == QLatin1String("look") || tag == QLatin1String("outfit")) {
            const auto a = xml.attributes();
            creature.lookType =
                a.value(QLatin1String("type")).toInt();
            if (creature.lookType == 0)
                creature.lookType =
                    a.value(QLatin1String("looktype")).toInt();
            creature.lookItem =
                a.value(QLatin1String("typeex")).toInt();
            if (creature.lookItem == 0)
                creature.lookItem =
                    a.value(QLatin1String("lookitem")).toInt();
            creature.lookHead = a.value(QLatin1String("head")).toInt();
            creature.lookBody = a.value(QLatin1String("body")).toInt();
            creature.lookLegs = a.value(QLatin1String("legs")).toInt();
            creature.lookFeet = a.value(QLatin1String("feet")).toInt();
            xml.skipCurrentElement();
        } else {
            xml.skipCurrentElement();
        }
    }
    if (xml.hasError() || creature.name.trimmed().isEmpty()) {
        if (error) {
            *error = xml.hasError() ? xml.errorString()
                                    : QStringLiteral("Creature name is missing.");
        }
        return false;
    }
    return true;
}

void collectCreatureImportFile(const QString &path,
                               QVector<CreatureStore::CreatureType> &imported,
                               QStringList &failures)
{
    if (QFileInfo(path).suffix().compare(QLatin1String("lua"), Qt::CaseInsensitive) == 0) {
        CreatureStore::CreatureType creature;
        QString error;
        if (!readLuaCreatureDefinition(path, creature, error)) {
            failures.append(QStringLiteral("%1: %2")
                                .arg(QFileInfo(path).fileName(), error.isEmpty()
                                    ? QStringLiteral("No monster type definition was found.")
                                    : error));
            return;
        }
        imported.append(std::move(creature));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        failures.append(QStringLiteral("%1: %2")
                            .arg(QFileInfo(path).fileName(), file.errorString()));
        return;
    }
    QXmlStreamReader xml(&file);
    if (!xml.readNextStartElement()) {
        failures.append(QStringLiteral("%1: Empty XML file.")
                            .arg(QFileInfo(path).fileName()));
        return;
    }

    const bool monsterIndex = xml.name().compare(
        QLatin1String("monsters"), Qt::CaseInsensitive) == 0;
    const bool npcIndex = xml.name().compare(
        QLatin1String("npcs"), Qt::CaseInsensitive) == 0;
    if (monsterIndex || npcIndex) {
        const QDir directory = QFileInfo(path).dir();
        while (xml.readNextStartElement()) {
            const QStringView expectedTag = npcIndex
                ? QStringView(u"npc") : QStringView(u"monster");
            if (xml.name().compare(expectedTag, Qt::CaseInsensitive) != 0) {
                xml.skipCurrentElement();
                continue;
            }
            const QString relative =
                xml.attributes().value(QLatin1String("file")).toString();
            const QString listedName =
                xml.attributes().value(QLatin1String("name")).toString();
            xml.skipCurrentElement();
            if (relative.isEmpty()) continue;
            CreatureStore::CreatureType creature;
            QString error;
            if (readCreatureDefinition(directory.filePath(relative), creature, &error)) {
                if (creature.name.isEmpty()) creature.name = listedName;
                if (npcIndex) creature.isNpc = true;
                imported.append(std::move(creature));
            } else {
                failures.append(QStringLiteral("%1: %2").arg(relative, error));
            }
        }
        return;
    }

    file.close();
    CreatureStore::CreatureType creature;
    QString error;
    if (readCreatureDefinition(path, creature, &error)) {
        imported.append(std::move(creature));
    } else {
        failures.append(QStringLiteral("%1: %2")
                            .arg(QFileInfo(path).fileName(), error));
    }
}
}

QVariantMap CreatureStore::importOtFile(const QString &pathOrUrl)
{
    QString path = QUrl(pathOrUrl).toLocalFile();
    if (path.isEmpty()) path = pathOrUrl;
    QVector<CreatureType> imported;
    QStringList failures;
    collectCreatureImportFile(path, imported, failures);
    return storeImported(std::move(imported), failures);
}

QVariantMap CreatureStore::importOtFiles(const QVariantList &pathsOrUrls)
{
    QVector<CreatureType> imported;
    QStringList failures;
    for (const QVariant &value : pathsOrUrls) {
        const QUrl url = value.toUrl();
        QString path = url.isLocalFile() ? url.toLocalFile() : value.toString();
        if (path.isEmpty()) continue;
        collectCreatureImportFile(path, imported, failures);
    }

    QVariantMap result = storeImported(std::move(imported), failures);
    result.insert(QStringLiteral("selected"), pathsOrUrls.size());
    return result;
}

QVariantMap CreatureStore::importOtDirectory(const QString &pathOrUrl)
{
    QString rootPath = QUrl(pathOrUrl).toLocalFile();
    if (rootPath.isEmpty()) rootPath = pathOrUrl;

    const QDir root(rootPath);
    if (!root.exists()) {
        const QString error = QStringLiteral("The selected server directory does not exist.");
        setErrorString(error);
        return {{QStringLiteral("success"), false},
                {QStringLiteral("error"), error}};
    }

    QVector<CreatureType> imported;
    QStringList failures;
    int candidateCount = 0;
    const QString rootName = QFileInfo(root.absolutePath()).fileName().toLower();
    const bool rootIsMonsterDirectory = rootName == QLatin1String("monster")
                                     || rootName == QLatin1String("monsters");
    const bool rootIsNpcDirectory = rootName == QLatin1String("npc")
                                 || rootName == QLatin1String("npcs");

    QDirIterator iterator(root.absolutePath(),
                          {QStringLiteral("*.xml"), QStringLiteral("*.lua")},
                          QDir::Files, QDirIterator::Subdirectories);
    QStringList files;
    while (iterator.hasNext()) files.append(iterator.next());
    std::sort(files.begin(), files.end(), [](const QString &left, const QString &right) {
        const bool leftLua = QFileInfo(left).suffix().compare(QLatin1String("lua"), Qt::CaseInsensitive) == 0;
        const bool rightLua = QFileInfo(right).suffix().compare(QLatin1String("lua"), Qt::CaseInsensitive) == 0;
        if (leftLua != rightLua) return !leftLua;
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    for (const QString &filePath : files) {
        const QFileInfo info(filePath);
        const QString baseName = info.fileName().toLower();
        if (baseName == QLatin1String("monsters.xml")
            || baseName == QLatin1String("npcs.xml")) {
            continue;
        }

        bool inMonsterDirectory = rootIsMonsterDirectory;
        bool inNpcDirectory = rootIsNpcDirectory;
        const QString relative = root.relativeFilePath(filePath)
                                     .replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (int i = 0; i + 1 < parts.size(); ++i) {
            const QString part = parts[i].toLower();
            inMonsterDirectory = inMonsterDirectory
                              || part == QLatin1String("monster")
                              || part == QLatin1String("monsters");
            inNpcDirectory = inNpcDirectory
                          || part == QLatin1String("npc")
                          || part == QLatin1String("npcs");
        }
        const bool isLua = info.suffix().compare(QLatin1String("lua"), Qt::CaseInsensitive) == 0;
        if (isLua && (!inMonsterDirectory || baseName.contains(QLatin1Char('#')))) continue;
        if (!inMonsterDirectory && !inNpcDirectory) continue;

        if (isLua) {
            CreatureType creature;
            QString error;
            if (!readLuaCreatureDefinition(filePath, creature, error)) {
                if (!error.isEmpty()) {
                    ++candidateCount;
                    failures.append(QStringLiteral("%1: %2").arg(relative, error));
                }
                continue;
            }
            ++candidateCount;
            imported.append(std::move(creature));
        } else {
            ++candidateCount;
            CreatureType creature;
            QString error;
            if (readCreatureDefinition(filePath, creature, &error)) {
                if (inNpcDirectory && !inMonsterDirectory) creature.isNpc = true;
                imported.append(std::move(creature));
            } else {
                failures.append(QStringLiteral("%1: %2").arg(relative, error));
            }
        }
    }

    if (candidateCount == 0) {
        const QString error = QStringLiteral(
            "No monster or NPC definitions were found. Select the server root, data folder, or a monster/NPC folder.");
        setErrorString(error);
        return {{QStringLiteral("success"), false},
                {QStringLiteral("error"), error}};
    }

    QVariantMap result = storeImported(std::move(imported), failures);
    result.insert(QStringLiteral("scanned"), candidateCount);
    return result;
}

QVariantMap CreatureStore::storeImported(QVector<CreatureType> imported,
                                         const QStringList &failures)
{
    QVariantMap result;
    if (imported.isEmpty()) {
        const QString error = failures.isEmpty()
                                  ? QStringLiteral("No creature definitions found.")
                                  : failures.join(QLatin1Char('\n'));
        setErrorString(error);
        result.insert(QStringLiteral("success"), false);
        result.insert(QStringLiteral("error"), error);
        return result;
    }

    const QVector<CreatureType> previous = m_creatures;
    beginResetModel();
    for (const CreatureType &creature : imported) {
        int row = -1;
        for (int i = 0; i < m_creatures.size(); ++i) {
            if (m_creatures[i].isNpc == creature.isNpc
                && m_creatures[i].name.compare(creature.name,
                                               Qt::CaseInsensitive) == 0) {
                row = i;
                break;
            }
        }
        if (row >= 0) m_creatures[row] = creature;
        else m_creatures.append(creature);
    }
    std::sort(m_creatures.begin(), m_creatures.end(),
              [](const CreatureType &a, const CreatureType &b) {
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });
    endResetModel();
    const bool saved = saveFile(m_creatures);
    if (!saved) {
        beginResetModel();
        m_creatures = previous;
        endResetModel();
    }
    emit countChanged();
    result.insert(QStringLiteral("success"), saved);
    result.insert(QStringLiteral("imported"), imported.size());
    result.insert(QStringLiteral("failed"), failures.size());
    result.insert(QStringLiteral("error"),
                  saved ? failures.join(QLatin1Char('\n')) : m_errorString);
    return result;
}

int CreatureStore::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_creatures.size());
}

QVariant CreatureStore::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_creatures.size())
        return {};
    const CreatureType &c = m_creatures[index.row()];
    switch (role) {
    case NameRole:     return c.name;
    case IsNpcRole:    return c.isNpc;
    case LookTypeRole: return c.lookType;
    case LookItemRole: return c.lookItem;
    case LookHeadRole: return c.lookHead;
    case LookBodyRole: return c.lookBody;
    case LookLegsRole: return c.lookLegs;
    case LookFeetRole: return c.lookFeet;
    default:           return {};
    }
}

QHash<int, QByteArray> CreatureStore::roleNames() const
{
    return {
        { NameRole,     QByteArrayLiteral("name") },
        { IsNpcRole,    QByteArrayLiteral("isNpc") },
        { LookTypeRole, QByteArrayLiteral("lookType") },
        { LookItemRole, QByteArrayLiteral("lookItem") },
        { LookHeadRole, QByteArrayLiteral("lookHead") },
        { LookBodyRole, QByteArrayLiteral("lookBody") },
        { LookLegsRole, QByteArrayLiteral("lookLegs") },
        { LookFeetRole, QByteArrayLiteral("lookFeet") },
    };
}
