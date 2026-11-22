#include "otbmreader.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

namespace {

bool check(bool passed, const char *message)
{
    if (!passed) qCritical().noquote() << message;
    return passed;
}

bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DME tests"));
    QCoreApplication::setApplicationName(QStringLiteral("BlackTek zones"));
    QTemporaryDir fixture;
    if (!fixture.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());

    const QString mapPath = fixture.filePath("world.otbm");
    const QString xmlPath = fixture.filePath("legacy.xml");
    const QString dir = fixture.filePath("world-zones");
    const QString zonesPath = dir + "/nested/floor-7.toml";
    const QByteArray original = "# retain this comment\n[Forest]\n"
        "range = { start = \"100:100:7\", end = \"110:110:7\" }\n"
        "spawn_type = \"monster\"\npolicy = \"fixed\"\n"
        "monsters = [{ name = \"Wolf\", position = \"1:2:0\" }]\n"
        "[Relative]\npositions = [\"105:105:7\"]\nspawn_type = \"monster\"\n"
        "policy = \"relative\"\nmonsters = [{ name = \"Wolf\", position = \"1:2:0\" }]\n";
    const auto invalidRules = BlackTekZones::parse("invalid.toml",
        "[InvalidNpc]\npositions = [\"100:100:7\"]\nnpcs = [{ interval = 5 }]\n"
        "[InvalidMonster]\npositions = [\"100:100:7\"]\n"
        "monsters = [{ name = \"Wolf\", position = \"bad\" }]\n"
        "[NoGeometry]\nrange = { start = \"bad\" }\n");
    if (!check(invalidRules.zones.isEmpty() && invalidRules.errors.size() == 3,
               "Report zones the server would reject")) return 1;

    const auto permissiveRules = BlackTekZones::parse("permissive.toml",
        "[Warn]\npositions = [\"100:100:7\", \"bad\"]\n"
        "spawn_type = \"boss\"\npolicy = \"staged\"\nflags = [\"no_death\", \"typo\"]\n"
        "trigger = \"typo\"\nweekdays = [\"Monday\"]\nforced = \"yes\"\ncooldown = 70\n"
        "monsters = [{ list = [{ name = \"Wolf\", chance = \"half\" }], interval = -1, wave = 257 }]\n"
        "minions = [{ name = \"Rat\", delay = 500 }]\n"
        "master = { name = \"Boss\" }\n");
    if (!check(permissiveRules.errors.isEmpty() && permissiveRules.zones.size() == 1
               && permissiveRules.warnings.size() == 10,
               "Warn when server defaults, skips, or wraps values without rejecting the zone")) return 1;
    const auto &creatures = permissiveRules.zones.front().creatures;
    if (!check(creatures.size() == 3 && creatures[0].name == QStringLiteral("Wolf")
               && creatures[0].intervalSeconds == 60 && creatures[0].wave == 1
               && creatures[1].delayMilliseconds == 500,
               "Use server defaults for optional fields and preserve delay units")) return 1;
    OtbmReader source;
    if (!check(source.newMap(256, 256, 1098, 3, 57)
               && source.setMapProperties("Fixture", 256, 256, "legacy.xml", "world-house.xml")
               && source.addItem(101, 102, 7, 100) && source.saveFile(mapPath),
               "Could not create OTBM fixture")) return 1;
    const QByteArray legacy = "<spawns><spawn centerx=\"101\" centery=\"102\" centerz=\"7\" radius=\"1\">"
                              "<monster name=\"Rat\" x=\"0\" y=\"0\" z=\"0\" spawntime=\"60\"/>"
                              "</spawn></spawns>";
    if (!check(write(xmlPath, legacy) && QDir().mkpath(dir + "/nested") && write(zonesPath, original),
               "Could not write zone and legacy fixtures")) return 1;

    OtbmReader standard;
    if (!check(standard.loadFile(mapPath), "Could not load fixture in standard mode")) return 1;
    const auto *standardTile = standard.tileAt(101, 102, 7);
    if (!check(standardTile && standardTile->creature_name == QStringLiteral("Rat"),
               "Standard mode must still load legacy XML spawns")) return 1;

    OtbmReader map;
    map.setBlackTekSpawnMode(true);
    if (!check(map.loadFile(mapPath) && map.blackTekZoneEntries().size() == 2,
               "BlackTek must find both nested TOML zones")) return 1;
    const auto *tile = map.tileAt(101, 102, 7);
    if (!check(tile && tile->creature_name.isEmpty(), "BlackTek must ignore authored XML creatures")) return 1;
    const auto geometry = map.blackTekZoneGeometry(zonesPath, "Forest");
    const auto positions = geometry.value("spawns").toList();
    if (!check(positions.size() == 1 && positions[0].toMap().value("x").toInt() == 101,
               "Fixed spawn should use a zone-relative position")) return 1;
    if (!check(map.blackTekZoneGeometry(zonesPath, "Relative").value("spawns").toList().isEmpty(),
               "Player-relative spawns must not show a fixed marker")) return 1;

    const auto renamed = map.changeBlackTekZoneName(QString::fromUtf8(original), "Forest", "Wooded Area");
    const auto renamedZones = BlackTekZones::parse(zonesPath, renamed.toUtf8());
    if (!check(!renamed.isEmpty() && renamed.startsWith("# retain this comment")
               && renamedZones.errors.isEmpty() && renamedZones.zones.size() == 2,
               "Rename must preserve the other zone and comments")) return 1;
    const auto removed = map.changeBlackTekZoneName(renamed, "Wooded Area", "");
    const auto removedZones = BlackTekZones::parse(zonesPath, removed.toUtf8());
    if (!check(removedZones.errors.isEmpty() && removedZones.zones.size() == 1
               && removedZones.zones.front().name == QStringLiteral("Relative"),
               "Removing a zone must preserve unrelated zones")) return 1;
    if (!check(!map.editBlackTekZoneFile(zonesPath, QStringLiteral("[broken")).isEmpty()
               && map.blackTekZoneFileText(zonesPath) == QString::fromUtf8(original),
               "Invalid TOML must not change document state")) return 1;

    const auto moved = map.replaceBlackTekZoneField(QString::fromUtf8(original), "Forest",
                                                    "monsters.0.position", "\"2:2:0\"");
    if (!check(!moved.isEmpty() && moved.startsWith("# retain this comment")
               && map.editBlackTekZoneFile(zonesPath, moved).isEmpty(),
               "Moving a spawn must preserve text outside the edited value")) return 1;
    if (!check(map.undo() && map.blackTekZoneFileText(zonesPath) == QString::fromUtf8(original)
               && map.redo() && map.blackTekZoneFileText(zonesPath) == moved,
               "Undo and redo must restore the TOML payload")) return 1;

    const auto recoveryPath = fixture.filePath("recovered.otbm");
    if (!check(OtbmReader::writeRecoverySnapshot(map.createRecoverySnapshot(), recoveryPath).isEmpty(),
               "Recovery must persist staged TOML")) return 1;
    OtbmReader recovered;
    recovered.setBlackTekSpawnMode(true, true);
    if (!check(recovered.loadFile(recoveryPath) && recovered.blackTekZoneFileText(zonesPath) == moved,
               "Recovery must load staged TOML from its snapshot")) return 1;

    const auto external = original + "# external edit\n";
    if (!check(write(zonesPath, external) && !map.saveFile(mapPath) && read(zonesPath) == external,
               "Save must stop on external changes")) return 1;
    if (!check(write(zonesPath, original) && map.saveFile(fixture.filePath("copy.otbm")),
               "Save As must create a separate zone folder")) return 1;
    if (!check(read(fixture.filePath("copy-zones/nested/floor-7.toml")) == moved.toUtf8()
               && read(zonesPath) == original && read(xmlPath) == legacy,
               "Save As must preserve original TOML and XML")) return 1;
    const auto customFolder = fixture.filePath("selected-layout");
    if (!check(QDir().mkpath(customFolder) && write(customFolder + "/custom.toml", original)
               && map.linkBlackTekZones(customFolder),
               "The user can link a folder without the world name")) return 1;
    OtbmReader reopened;
    reopened.setBlackTekSpawnMode(true);
    if (!check(reopened.loadFile(fixture.filePath("copy.otbm"))
               && reopened.blackTekZoneFolder() == customFolder && reopened.blackTekZoneEntries().size() == 2,
               "The custom folder must be remembered for that map")) return 1;
    return 0;
}
