#include "blacktekzones.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <sstream>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>

namespace {
bool parsePosition(const QString &text, int &x, int &y, int &z)
{
    auto input = text.toStdString();
    std::replace(input.begin(), input.end(), ':', ' ');
    std::istringstream stream(input);
    uint16_t parsedX = 0, parsedY = 0, parsedZ = 0;
    char trailing;
    if (!(stream >> parsedX >> parsedY >> parsedZ) || (stream >> trailing) || parsedZ > 255)
        return false;
    x = parsedX;
    y = parsedY;
    z = parsedZ;
    return true;
}

using Choices = std::initializer_list<std::string_view>;
constexpr uint32_t maxIntervalSeconds = std::numeric_limits<uint32_t>::max() / 1000u;

class ZoneDiagnostics
{
public:
    ZoneDiagnostics(QStringList &warnings, const QString &path, const QString &zone)
        : m_warnings(warnings), m_prefix(path + QStringLiteral(": ") + zone + QStringLiteral(": ")) {}

    void warn(const QString &message) const
    {
        m_warnings.append(m_prefix + QStringLiteral("Warning: ") + message);
    }

    void checkChoice(const toml::node &node, const QString &field, Choices choices) const
    {
        const auto value = node.value<std::string_view>();
        if (!value || std::find(choices.begin(), choices.end(), *value) == choices.end())
            warn(QStringLiteral("%1 has an unrecognized value that the server ignores").arg(field));
    }

    void checkChoice(const toml::table &table, const char *key, Choices choices) const
    {
        if (const auto *node = table.get(key))
            checkChoice(*node, QString::fromLatin1(key), choices);
    }

    void checkList(const toml::table &table, const char *key, Choices choices) const
    {
        const auto *node = table.get(key);
        if (!node) return;
        const QString field = QString::fromLatin1(key);
        if (const auto *entries = node->as_array()) {
            qsizetype index = 0;
            for (const auto &entry : *entries)
                checkChoice(entry, QStringLiteral("%1[%2]").arg(field).arg(++index), choices);
        } else {
            warn(QStringLiteral("%1 is ignored because it is not an array").arg(field));
        }
    }

    uint32_t unsignedField(const toml::table &table, const char *key, uint32_t fallback,
                           const QString &context = {}) const
    {
        const auto *node = table.get(key);
        if (!node) return fallback;
        if (const auto value = node->value<uint32_t>()) return *value;
        const QString field = context + QString::fromLatin1(key);
        warn(QStringLiteral("%1 is not a valid unsigned 32-bit integer; the server uses %2")
             .arg(field).arg(fallback));
        return fallback;
    }

private:
    QStringList &m_warnings;
    QString m_prefix;
};

void validateRules(const toml::table &table, const ZoneDiagnostics &diagnostics)
{
    diagnostics.checkChoice(table, "spawn_type", {"monster", "npc", "boss", "item"});
    diagnostics.checkChoice(table, "policy", {"fixed", "relative", "staged", "triggered"});
    diagnostics.checkChoice(table, "trigger", {
        "Spawn", "Enter", "Leave", "Death", "Despawn", "Login", "Logout", "Use", "Remove",
        "Transform", "Speak", "Look", "Summon", "Equip", "DeEquip", "Attack", "Defend", "Idle",
        "GainSkull", "LoseSkull", "Kill", "EnterBattle", "LeaveBattle"
    });
    diagnostics.checkChoice(table, "minion_behavior", {
        "despawn_on_master_death", "finish_life_only", "independent_respawn"
    });
    diagnostics.checkList(table, "flags", {
        "protection", "nopvp", "pvp", "nologout", "no_experience", "no_trading", "no_transaction",
        "no_party", "no_drop", "no_death", "no_death_penalty", "no_equipment_loss", "no_summons",
        "ghost", "no_pathfinding", "no_walk", "no_monsters", "no_npcs"
    });
    static const Choices weekdays{"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};
    diagnostics.checkList(table, "weekdays", weekdays);
    const auto *schedule = table["weekdays"].as_array();
    if (schedule && !schedule->empty()
        && std::none_of(schedule->begin(), schedule->end(), [&](const auto &day) {
            const auto name = day.template value<std::string_view>();
            return name && std::find(weekdays.begin(), weekdays.end(), *name) != weekdays.end();
        }))
        diagnostics.warn(QStringLiteral("The weekday list enables no days"));
    for (const char *key : {"passive", "forced", "rebootable", "resumable", "degradable", "timed"}) {
        const auto *node = table.get(key);
        if (node && !node->value<bool>())
            diagnostics.warn(QStringLiteral("%1 is not a boolean; the server uses false").arg(QString::fromLatin1(key)));
    }
    if (diagnostics.unsignedField(table, "cooldown", 5u) > std::numeric_limits<uint16_t>::max() / 1000u)
        diagnostics.warn(QStringLiteral("cooldown wraps in the server's 16-bit millisecond timer"));

    const auto type = table["spawn_type"].value_or(std::string_view{});
    const auto policy = table["policy"].value_or(std::string_view{});
    if (type == "npc" && (policy == "staged" || policy == "triggered"))
        diagnostics.warn(QStringLiteral("NPC spawns require fixed or relative policy to run"));
    else if (type == "item")
        diagnostics.warn(QStringLiteral("This server recognizes item zones but does not run an item spawn policy"));
    else if ((type == "monster" || type == "boss" || type == "npc") && policy.empty())
        diagnostics.warn(QStringLiteral("The spawn type has no policy and will not run"));
    if ((type == "monster" || type == "boss") && policy == "triggered" && !table.contains("trigger"))
        diagnostics.warn(QStringLiteral("Triggered spawns have no trigger configured"));
    if (type == "boss" && !table["master"].is_table())
        diagnostics.warn(QStringLiteral("Boss zone has no master entry"));
}

QString collectEntries(const toml::table &table, BlackTekZone &zone, const ZoneDiagnostics &diagnostics)
{
    const auto read = [&](const toml::node &node, const QString &category, int index) -> QString {
        const QString label = category == QStringLiteral("master") ? category
            : QStringLiteral("%1[%2]").arg(category).arg(index + 1);
        const auto context = label + QLatin1Char('.');
        const auto *entry = node.as_table();
        if (!entry) {
            diagnostics.warn(QStringLiteral("%1 is not a table; the server creates an empty entry").arg(label));
            return {};
        }
        const bool npc = category == QStringLiteral("npcs");
        const auto name = (*entry)["name"].value<std::string_view>();
        if (npc && !name) return QStringLiteral("%1 requires a string name").arg(label);

        BlackTekZone::CreatureEntry creature;
        creature.category = category;
        creature.index = index;
        QStringList names;
        const auto appendName = [&](std::string_view name, const QString &field) {
            if (!npc && name.find_first_of(":;") != std::string_view::npos)
                diagnostics.warn(QStringLiteral("%1 name contains a character used as a server weight delimiter").arg(field));
            names.append(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
        };
        const auto *list = npc ? nullptr : entry->get("list");
        if (list) {
            if (const auto *options = list->as_array()) {
                int optionIndex = 0;
                for (const auto &option : *options) {
                    const auto optionLabel = QStringLiteral("%1list[%2]").arg(context).arg(++optionIndex);
                    const auto *choice = option.as_table();
                    const auto choiceName = choice ? (*choice)["name"].value<std::string_view>() : std::nullopt;
                    if (!choiceName || choiceName->empty()) {
                        diagnostics.warn(QStringLiteral("%1 has no usable name").arg(optionLabel));
                        continue;
                    }
                    appendName(*choiceName, optionLabel);
                    const auto weight = diagnostics.unsignedField(*choice, "chance", 100u, optionLabel + QLatin1Char('.'));
                    if (weight > std::numeric_limits<uint16_t>::max())
                        diagnostics.warn(QStringLiteral("%1chance exceeds the server's 16-bit weight")
                                         .arg(optionLabel + QLatin1Char('.')));
                }
            } else {
                diagnostics.warn(QStringLiteral("%1list must be an array; its presence overrides name").arg(context));
            }
        } else if (name && !name->empty()) {
            appendName(*name, label);
        }
        if (names.isEmpty())
            diagnostics.warn(QStringLiteral("%1 has no usable creature name").arg(label));
        creature.name = names.join(QStringLiteral(" / "));
        creature.names = std::move(names);
        creature.intervalSeconds = diagnostics.unsignedField(*entry, "interval", npc ? 0u : 60u, context);
        if (creature.intervalSeconds > maxIntervalSeconds)
            diagnostics.warn(QStringLiteral("%1interval wraps in the server's millisecond timer").arg(context));
        if (!npc)
            creature.chance = diagnostics.unsignedField(*entry, "chance", 100u, context);
        if (category == QStringLiteral("minions"))
            creature.delayMilliseconds = diagnostics.unsignedField(*entry, "delay", 0u, context);
        if (zone.staged && category == QStringLiteral("monsters")) {
            const auto wave = diagnostics.unsignedField(*entry, "wave", 1u, context);
            if (wave > std::numeric_limits<uint8_t>::max())
                diagnostics.warn(QStringLiteral("%1wave wraps in the server's 8-bit wave field").arg(context));
            creature.wave = static_cast<uint8_t>(wave);
        }

        if (const auto *position = entry->get("position")) {
            if (const auto value = position->value<std::string>()) {
                int x = 0, y = 0, z = 0;
                if (!parsePosition(QString::fromStdString(*value), x, y, z))
                    return QStringLiteral("Invalid %1position").arg(context);
                creature.hasPosition = true;
                const auto origin = zone.hasRange ? zone.rangeStart
                                                  : BlackTekZone::Position3D{zone.minX, zone.minY, zone.floor};
                if (!zone.playerRelative && (origin.x + x > 65535 || origin.y + y > 65535 || origin.z + z > 255))
                    diagnostics.warn(QStringLiteral("%1position wraps when added to the zone origin").arg(context));
                creature.position = zone.playerRelative
                    ? BlackTekZone::Position3D{x, y, z}
                    : BlackTekZone::Position3D{static_cast<uint16_t>(origin.x + x),
                                              static_cast<uint16_t>(origin.y + y),
                                              static_cast<uint8_t>(origin.z + z)};
            } else {
                diagnostics.warn(QStringLiteral("%1position is not a string; the server uses 0:0:0").arg(context));
            }
        }
        zone.creatures.append(std::move(creature));
        return {};
    };
    for (const char *category : {"monsters", "npcs", "minions"}) {
        const auto *node = table.get(category);
        if (!node) continue;
        if (const auto *entries = node->as_array()) {
            int index = 0;
            for (const auto &entry : *entries)
                if (const QString error = read(entry, QString::fromLatin1(category), index++); !error.isEmpty())
                    return error;
        } else {
            diagnostics.warn(QStringLiteral("%1 is ignored because it is not an array").arg(QString::fromLatin1(category)));
        }
    }
    if (const auto *master = table.get("master")) {
        if (master->is_table()) return read(*master, QStringLiteral("master"), 0);
        diagnostics.warn(QStringLiteral("master is ignored because it is not a table"));
    }
    return {};
}

}

BlackTekZoneLoadResult BlackTekZones::parse(const QString &path, const QByteArray &bytes)
{
    BlackTekZoneLoadResult result;
    try {
        const toml::table file = toml::parse(std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())));
        for (const auto &[name, node] : file) {
            const auto *table = node.as_table();
            if (!table) continue;
            BlackTekZone zone;
            zone.name = QString::fromUtf8(name.str().data(), static_cast<qsizetype>(name.str().size()));
            zone.sourceFile = path;
            const auto spawnType = (*table)["spawn_type"].value_or(std::string_view{});
            if (spawnType == "monster" || spawnType == "npc" || spawnType == "boss" || spawnType == "item")
                zone.spawnType = QString::fromUtf8(spawnType.data(), static_cast<qsizetype>(spawnType.size()));
            if (const auto *flags = (*table)["flags"].as_array()) {
                zone.hasFlags = true;
                for (const auto &flag : *flags) {
                    const auto value = flag.value_or(std::string_view{});
                    if (value.empty()) continue;
                    const QString name = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
                    if (!zone.flags.contains(name)) zone.flags.append(name);
                    if (name == QLatin1String("protection")) zone.protection = true;
                }
            } else if (table->contains("flags")) zone.hasFlags = true;
            const ZoneDiagnostics diagnostics(result.warnings, path, zone.name);
            validateRules(*table, diagnostics);
            const auto policy = (*table)["policy"].value_or(std::string_view{});
            zone.playerRelative = policy == "relative";
            zone.staged = policy == "staged";
            if (auto range = table->get_as<toml::table>("range")) {
                zone.hasRange = parsePosition(QString::fromStdString((*range)["start"].value<std::string>().value_or("")), zone.rangeStart.x, zone.rangeStart.y, zone.rangeStart.z)
                    && parsePosition(QString::fromStdString((*range)["end"].value<std::string>().value_or("")), zone.rangeEnd.x, zone.rangeEnd.y, zone.rangeEnd.z);
                if (zone.hasRange) {
                    zone.minX = std::min(zone.rangeStart.x, zone.rangeEnd.x);
                    zone.maxX = std::max(zone.rangeStart.x, zone.rangeEnd.x);
                    zone.minY = std::min(zone.rangeStart.y, zone.rangeEnd.y);
                    zone.maxY = std::max(zone.rangeStart.y, zone.rangeEnd.y);
                    zone.floor = std::min(zone.rangeStart.z, zone.rangeEnd.z);
                }
            }
            if (table->contains("range") && !zone.hasRange)
                diagnostics.warn(QStringLiteral("range is ignored because it lacks valid start and end coordinates"));
            if (const auto *positions = (*table)["positions"].as_array()) {
                zone.hasPositions = true;
                qsizetype index = 0;
                for (const auto &position : *positions) {
                    int x = 0, y = 0, z = 0;
                    ++index;
                    if (auto value = position.value<std::string>(); value && parsePosition(QString::fromStdString(*value), x, y, z)) {
                        zone.worldPositions.append({x, y, z});
                    } else {
                        diagnostics.warn(QStringLiteral("positions[%1] is ignored because its coordinates are invalid").arg(index));
                    }
                }
            } else if (table->contains("positions")) {
                zone.hasPositions = true;
                diagnostics.warn(QStringLiteral("positions is ignored because it is not an array"));
            }
            if (!zone.hasRange && zone.worldPositions.isEmpty()) {
                result.errors.append(QStringLiteral("%1: %2 has no valid range or positions").arg(path, zone.name));
                continue;
            }
            if (!zone.hasRange && !zone.worldPositions.isEmpty()) {
                zone.minX = zone.worldPositions.front().x;
                zone.minY = zone.worldPositions.front().y;
                zone.floor = zone.worldPositions.front().z;
                for (const auto &position : zone.worldPositions) {
                    zone.minX = std::min(zone.minX, position.x);
                    zone.minY = std::min(zone.minY, position.y);
                    zone.floor = std::min(zone.floor, position.z);
                }
            }
            if (const auto error = collectEntries(*table, zone, diagnostics); !error.isEmpty()) {
                result.errors.append(QStringLiteral("%1: %2: %3").arg(path, zone.name, error));
                continue;
            }
            result.zones.append(std::move(zone));
        }
    } catch (const toml::parse_error &error) {
        result.errors.append(path + QStringLiteral(": ") + QString::fromUtf8(error.description().data(), static_cast<qsizetype>(error.description().size())));
    }
    return result;
}
