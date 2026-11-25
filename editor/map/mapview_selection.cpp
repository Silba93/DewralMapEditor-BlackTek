
#include "mapview.h"
#include "mapview_p.h"
#include "mapselectionrotation.h"

#include <QPainter>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QWheelEvent>
#include <QCursor>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QTimer>
#include <QGuiApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <tuple>
#include <vector>

namespace {
bool appendTomlString(QString &out, const QString &value)
{
    out += QLatin1Char('"');
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (code < 0x20 || code == 0x7f) return false;
        if (character == QLatin1Char('"') || character == QLatin1Char('\\'))
            out += QLatin1Char('\\');
        out += character;
    }
    out += QLatin1Char('"');
    return true;
}

QString tomlString(const QString &value)
{
    const QByteArray encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
}

QString creatureEntryAtPosition(QString entry, qsizetype positionStart,
                                qsizetype positionEnd, int x, int y, int z)
{
    const QString value = tomlString(QStringLiteral("%1:%2:%3").arg(x).arg(y).arg(z));
    if (positionStart >= 0 && positionEnd > positionStart && positionEnd <= entry.size()) {
        entry.replace(positionStart, positionEnd - positionStart, value);
        return entry;
    }
    const qsizetype opening = entry.indexOf(QLatin1Char('{'));
    if (opening < 0) return {};
    entry.insert(opening + 1, QStringLiteral(" position = %1, ").arg(value));
    return entry;
}

bool isValidZoneFlag(const QString &flag)
{
    static const QSet<QString> allowed{
        QStringLiteral("protection"), QStringLiteral("nopvp"), QStringLiteral("pvp"),
        QStringLiteral("nologout"), QStringLiteral("no_experience"), QStringLiteral("no_trading"),
        QStringLiteral("no_transaction"), QStringLiteral("no_party"), QStringLiteral("no_drop"),
        QStringLiteral("no_death"), QStringLiteral("no_death_penalty"), QStringLiteral("no_equipment_loss"),
        QStringLiteral("no_summons"), QStringLiteral("ghost"), QStringLiteral("no_pathfinding"),
        QStringLiteral("no_walk"), QStringLiteral("no_monsters"), QStringLiteral("no_npcs")
    };
    return allowed.contains(flag);
}

bool isValidZoneTrigger(const QString &trigger)
{
    static const QSet<QString> allowed{
        QStringLiteral("Spawn"), QStringLiteral("Enter"), QStringLiteral("Leave"),
        QStringLiteral("Death"), QStringLiteral("Despawn"), QStringLiteral("Login"),
        QStringLiteral("Logout"), QStringLiteral("Use"), QStringLiteral("Remove"),
        QStringLiteral("Transform"), QStringLiteral("Speak"), QStringLiteral("Look"),
        QStringLiteral("Summon"), QStringLiteral("Equip"), QStringLiteral("DeEquip"),
        QStringLiteral("Attack"), QStringLiteral("Defend"), QStringLiteral("Idle"),
        QStringLiteral("GainSkull"), QStringLiteral("LoseSkull"), QStringLiteral("Kill"),
        QStringLiteral("EnterBattle"), QStringLiteral("LeaveBattle")
    };
    return allowed.contains(trigger);
}
}

void MapView::deleteSelectedTop()
{
    if (!m_otbm || m_selectionController.selected().isEmpty()) return;
    QVariantList blackTekRemovals;
    QSet<quint64> removedBlackTekPositions;
    if (m_otbm->blackTekSpawnMode()) {
        for (quint64 key : m_selectionController.selected()) {
            if (!m_selectionController.wholeStack()
                && m_selectionController.entity(key) != -2) continue;
            const QVariantMap entry = m_otbm->blackTekCreatureEntryAt(selX(key), selY(key), selZ(key));
            if (entry.isEmpty()) continue;
            QVariantMap operation = entry;
            operation.insert(QStringLiteral("action"), QStringLiteral("remove"));
            blackTekRemovals.append(operation);
            removedBlackTekPositions.insert(key);
        }
    }
    bool any = false;
    {
        std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
        m_otbm->beginUndoGroup();
        if (!blackTekRemovals.isEmpty()) {
            const QString error = m_otbm->editBlackTekCreatureEntries(blackTekRemovals);
            if (!error.isEmpty()) {
                m_otbm->cancelUndoGroup();
                emit operationWarning(error);
                return;
            }
            any = true;
        }
        for (quint64 key : m_selectionController.selected()) {
            const int x = selX(key), y = selY(key), z = selZ(key);

            bool removedHere = false;
            if (m_selectionController.wholeStack()) {
                if (m_otbm->setTileZoneIds(x, y, z, TileZoneIds())) removedHere = true;
                while (m_otbm->removeTopItem(x, y, z)) removedHere = true;
                if (m_otbm->clearCreatureAt(x, y, z)) removedHere = true;
                if (m_otbm->clearSpawnAt(x, y, z)) {
                    removedHere = true;
                    invalidateSpawnIndex();
                }
            } else if (m_selectionController.entity(key) >= 0) {
                removedHere = m_otbm->removeItemAt(x, y, z, m_selectionController.entity(key));
            } else if (m_selectionController.entity(key) == -2) {
                // BlackTek entries were staged above. A pixel-perfect or
                // context-picked native creature shares this entity id, so
                // clear its tile representation when no BlackTek entry exists.
                if (!removedBlackTekPositions.contains(key)
                    && m_otbm->blackTekCreatureEntryAt(x, y, z).isEmpty())
                    removedHere = m_otbm->clearCreatureAt(x, y, z);
            } else if (m_selectionController.entity(key) == -3) {
                removedHere = m_otbm->clearSpawnAt(x, y, z);
                if (removedHere) invalidateSpawnIndex();
            } else if (m_otbm->clearCreatureAt(x, y, z)) {
                removedHere = true;
            } else if (m_otbm->clearSpawnAt(x, y, z)) {
                removedHere = true;
                invalidateSpawnIndex();
            } else if (m_otbm->removeTopItem(x, y, z)) {
                removedHere = true;
            }
            if (removedHere) { any = true; onTileEdited(x, y, z); }
        }
        m_otbm->endUndoGroup();
        flushEditedChunksLocked();
    }
    if (any) {
        if (!m_selectionController.entities.isEmpty()) clearSelection();
        refreshAfterEdit(0);
    }
}

void MapView::copySelection()
{
    if (!m_otbm || m_selectionController.selected().isEmpty()) return;

    bool first = true;
    int minX = 0, minY = 0;
    for (quint64 key : m_selectionController.selected()) {
        const int x = selX(key), y = selY(key);
        if (first) { minX = x; minY = y; first = false; }
        else { minX = std::min(minX, x); minY = std::min(minY, y); }
    }

    std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
    m_selectionController.clipboard().clear();
    for (quint64 key : m_selectionController.selected()) {
        const int x = selX(key), y = selY(key), z = selZ(key);
        const OtbmTile *t = m_otbm->tileAt(x, y, z);
        ClipTile ct;
        ct.dx = x - minX;
        ct.dy = y - minY;

        ct.dz = z - m_navigationController.floor();

        const bool creatureOnly = !m_selectionController.wholeStack()
            && m_selectionController.entity(key) == -2;
        if (m_otbm->blackTekSpawnMode()
            && (m_selectionController.wholeStack() || creatureOnly)) {
            const QVariantMap entry = m_otbm->blackTekCreatureEntryAt(x, y, z);
            if (!entry.isEmpty()) {
                ct.blackTekCreature = true;
                ct.blackTekCategory = entry.value(QStringLiteral("category")).toString();
                ct.blackTekRawEntry = entry.value(QStringLiteral("rawEntry")).toString();
                ct.blackTekPositionStart = entry.value(QStringLiteral("positionStart")).toInt();
                ct.blackTekPositionEnd = entry.value(QStringLiteral("positionEnd")).toInt();
                if (const auto *creature = blackTekCreatureAt(x, y, z)) {
                    ct.creature = creature->name;
                    ct.npc = creature->isNpc;
                }
            }
        }
        if (creatureOnly && !ct.blackTekCreature
            && (!t || t->creature_name.isEmpty())) continue;
        if (!t) {
            if (ct.blackTekCreature) m_selectionController.clipboard().push_back(std::move(ct));
            continue;
        }

        if (m_selectionController.wholeStack()) {
            ct.items.assign(t->items.begin(), t->items.end());
            ct.creature = t->creature_name;
            ct.monsterChoices = m_otbm->monsterChoicesAt(t->x,t->y,t->z);
            ct.spawntime = t->creature_spawntime;
            ct.npc = t->creature_is_npc;
            ct.direction = t->creature_direction;
            ct.spawnRadius = t->spawn_radius;
            ct.npcRadius = m_otbm->npcSpawnRadiusAt(t->x,t->y,t->z);
            ct.zoneIds = t->zone_ids;
        } else if (m_selectionController.entity(key) >= 0) {
            const int index = m_selectionController.entity(key);
            if (size_t(index) < t->items.size()) ct.items.push_back(t->items[size_t(index)]);
        } else if (m_selectionController.entity(key) == -3) {
            ct.spawnRadius = t->spawn_radius;
            ct.npcRadius = m_otbm->npcSpawnRadiusAt(t->x,t->y,t->z);
        } else if ((!creatureOnly || !ct.blackTekCreature)
                   && !t->creature_name.isEmpty()
                   && m_selectionController.entity(key) != -3) {
            ct.creature = t->creature_name;
            ct.monsterChoices = m_otbm->monsterChoicesAt(t->x,t->y,t->z);
            ct.spawntime = t->creature_spawntime;
            ct.npc = t->creature_is_npc;
            ct.direction = t->creature_direction;
        } else if (!creatureOnly && !t->items.empty()) {
            ct.items.push_back(t->items.back());
        }
        if (ct.items.empty() && ct.creature.isEmpty() && !ct.blackTekCreature
            && ct.spawnRadius == 0 && ct.npcRadius == 0 && ct.zoneIds.empty()) continue;
        m_selectionController.clipboard().push_back(std::move(ct));
    }
    emit clipboardChanged();
}

QVariantList MapView::selectionItemIds()
{
    if (!m_otbm) return {};
    QSet<int> uniqueIds;
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    for (quint64 key : m_selectionController.selected()) {
        const auto *tile = m_otbm->tileAt(selX(key), selY(key), selZ(key));
        if (!tile) continue;
        for (const auto &item : tile->items)
            if (item.server_id > 0) uniqueIds.insert(int(item.server_id));
    }
    auto ids = uniqueIds.values();
    std::sort(ids.begin(), ids.end());
    QVariantList result;
    result.reserve(ids.size());
    for (int id : ids) result.append(id);
    return result;
}

QVariantMap MapView::brushSelectionSnapshot(bool includeGround)
{
    const auto &selection = m_selectionController.selected();
    if (!m_otbm || selection.isEmpty())
        return {{"error", "Select some map tiles first."}};
    if (selection.size() > 4096)
        return {{"error", "Select at most 4096 tiles for brush learning."}};
    int minX = 65535, minY = 65535, minZ = 15;
    for (quint64 key : selection) {
        minX = std::min(minX, selX(key));
        minY = std::min(minY, selY(key));
        minZ = std::min(minZ, selZ(key));
    }
    auto keys = selection.values();
    std::sort(keys.begin(), keys.end());
    QVariantList tiles;
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    for (quint64 key : keys) {
        const auto *tile = m_otbm->tileAt(selX(key), selY(key), selZ(key));
        if (!tile) continue;
        QVariantList ids;
        for (const auto &item : tile->items) {
            if (!item.server_id) continue;
            if (!includeGround && ((m_otb && m_otb->isClientGroundForServerId(item.server_id))
                || (m_brushController.store()
                    && m_brushController.store()->isGroundBrushItem(item.server_id)))) continue;
            ids.append(int(item.server_id));
        }
        if (!ids.isEmpty()) tiles.append(QVariantMap{{"dx", selX(key) - minX},
            {"dy", selY(key) - minY}, {"dz", selZ(key) - minZ}, {"items", ids}});
    }
    if (tiles.isEmpty()) return {{"error", "No items remain (try Include ground)."}};
    return {{"tiles", tiles}};
}

QVariantMap MapView::saveSelectionAsPrefab(const QString &name,
                                           const QString &palette)
{
    QVariantMap result;
    result.insert(QStringLiteral("success"), false);
    if (!m_otbm || !m_brushController.store()) {
        result.insert(QStringLiteral("error"), QStringLiteral("No map or brush profile is loaded."));
        return result;
    }
    if (m_selectionController.selected().isEmpty()) {
        result.insert(QStringLiteral("error"), QStringLiteral("Select at least one tile first."));
        return result;
    }
    if (name.trimmed().isEmpty()) {
        result.insert(QStringLiteral("error"), QStringLiteral("Enter a prefab name."));
        return result;
    }
    if (m_brushController.store()->isDoodadBrush(name.trimmed())) {
        result.insert(QStringLiteral("error"), QStringLiteral("A brush or prefab with this name already exists."));
        return result;
    }

    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int minZ = std::numeric_limits<int>::max();
    for (quint64 key : m_selectionController.selected()) {
        minX = std::min(minX, selX(key));
        minY = std::min(minY, selY(key));
        minZ = std::min(minZ, selZ(key));
    }

    QVariantList tiles;
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    for (quint64 key : m_selectionController.selected()) {
        const int x = selX(key), y = selY(key), z = selZ(key);
        const OtbmTile *tile = m_otbm->tileAt(x, y, z);
        if (!tile || tile->items.empty()) continue;
        QVariantList items;
        items.reserve(static_cast<qsizetype>(tile->items.size()));
        for (const OtbmMapItem &item : tile->items)
            if (item.server_id > 0) items.append(static_cast<int>(item.server_id));
        if (items.isEmpty()) continue;
        QVariantMap entry;
        entry.insert(QStringLiteral("dx"), x - minX);
        entry.insert(QStringLiteral("dy"), y - minY);
        entry.insert(QStringLiteral("dz"), z - minZ);
        entry.insert(QStringLiteral("items"), items);
        tiles.append(entry);
    }
    if (tiles.isEmpty()) {
        result.insert(QStringLiteral("error"), QStringLiteral("The selection contains no items."));
        return result;
    }
    if (!m_brushController.store()->savePrefab(name, palette, tiles)) {
        result.insert(QStringLiteral("error"), QStringLiteral("Could not save brushes.json."));
        return result;
    }
    result.insert(QStringLiteral("success"), true);
    result.insert(QStringLiteral("tileCount"), tiles.size());
    return result;
}

void MapView::cutSelection()
{
    copySelection();
    if (m_selectionController.clipboard().empty() || m_selectionController.selected().isEmpty()) return;
    QVariantList blackTekRemovals;
    if (m_otbm->blackTekSpawnMode()) {
        for (quint64 key : m_selectionController.selected()) {
            if (!m_selectionController.wholeStack()
                && m_selectionController.entity(key) != -2) continue;
            const QVariantMap entry = m_otbm->blackTekCreatureEntryAt(selX(key), selY(key), selZ(key));
            if (entry.isEmpty()) continue;
            QVariantMap operation = entry;
            operation.insert(QStringLiteral("action"), QStringLiteral("remove"));
            blackTekRemovals.append(operation);
        }
    }

    std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
    beginEditBatch();
    m_otbm->beginUndoGroup();
    bool any = false;
    if (!blackTekRemovals.isEmpty()) {
        const QString error = m_otbm->editBlackTekCreatureEntries(blackTekRemovals);
        if (!error.isEmpty()) {
            m_otbm->cancelUndoGroup();
            endEditBatch();
            emit operationWarning(error);
            return;
        }
        any = true;
    }
    for (quint64 key : m_selectionController.selected()) {
        const int x = selX(key), y = selY(key), z = selZ(key);
        bool removedHere = false;
        if (m_selectionController.wholeStack()) {

            while (m_otbm->removeTopItem(x, y, z)) { removedHere = true; any = true; }
            if (m_otbm->clearCreatureAt(x, y, z)) { removedHere = true; any = true; }
            if (m_otbm->clearSpawnAt(x, y, z)) { removedHere = true; any = true; invalidateSpawnIndex(); }
        } else {

            const int entity = m_selectionController.entity(key);
            if (entity >= 0) { removedHere = m_otbm->removeItemAt(x, y, z, entity); any |= removedHere; }
            else if (entity == -3) {
                removedHere = m_otbm->clearSpawnAt(x, y, z); any |= removedHere;
                if (removedHere) invalidateSpawnIndex();
            }
            else if (entity == -2) { /* BlackTek TOML removal was staged above. */ }
            else if (m_otbm->clearCreatureAt(x, y, z)) { removedHere = true; any = true; }
            else if (m_otbm->removeTopItem(x, y, z)) { removedHere = true; any = true; }
        }
        if (removedHere) onTileEdited(x, y, z);
    }
    m_otbm->endUndoGroup();
    endEditBatch();
    if (any) {
        if (!m_selectionController.entities.isEmpty()) clearSelection();
        refreshAfterEdit(0);
    }
}

void MapView::moveSelection(int dx, int dy, int dz)
{
    transformSelection(dx, dy, dz, 0);
}

void MapView::rotateSelection(int quarterTurns)
{
    transformSelection(0, 0, 0, ((quarterTurns % 4) + 4) % 4);
}

void MapView::transformSelection(int dx, int dy, int dz, int quarterTurns)
{
    if (!m_otbm || m_selectionController.selected().isEmpty()
        || (dx == 0 && dy == 0 && dz == 0 && quarterTurns == 0)) return;
    std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
    int minX = 65535, minY = 65535, maxX = 0, maxY = 0;
    for (quint64 key : m_selectionController.selected()) {
        minX = std::min(minX, selX(key)); maxX = std::max(maxX, selX(key));
        minY = std::min(minY, selY(key)); maxY = std::max(maxY, selY(key));
    }

    struct Snap {
        int x, y, z, nx, ny;
        std::vector<OtbmMapItem> items;
        QString creature; int spawntime = 60; bool npc = false; uint8_t direction = 0;
        int spawnRadius = 0;
        int npcRadius = 0;
        QVector<OtbmSpawnAlternative> monsterChoices;
        int entity = -1;
        int targetEntity = -1;
        TileZoneIds zoneIds;
        QVariantMap blackTekEntry;
        QString blackTekCategory;
        QString blackTekRawEntry;
        int blackTekPositionStart = -1;
        int blackTekPositionEnd = -1;
    };
    std::vector<Snap> snap;
    for (quint64 key : m_selectionController.selected()) {
        const int x = selX(key), y = selY(key), z = selZ(key);
        const OtbmTile *t = m_otbm->tileAt(x, y, z);
        const bool includeBlackTek = m_otbm->blackTekSpawnMode()
            && (m_selectionController.wholeStack() || m_selectionController.entity(key) == -2);
        const QVariantMap blackTekEntry = includeBlackTek
            ? m_otbm->blackTekCreatureEntryAt(x, y, z) : QVariantMap{};
        if (!t && blackTekEntry.isEmpty()) continue;
        Snap s; s.x = x; s.y = y; s.z = z;
        s.entity = m_selectionController.wholeStack() ? -1 : m_selectionController.entity(key);
        s.nx = x + dx; s.ny = y + dy;
        if (quarterTurns != 0) {
            const QPoint target = rotatedSelectionPosition(QPoint(x, y),
                QRect(QPoint(minX, minY), QPoint(maxX, maxY)), quarterTurns);
            s.nx = target.x(); s.ny = target.y();
        }
        if (!blackTekEntry.isEmpty()) {
            s.blackTekEntry = blackTekEntry;
            s.blackTekCategory = blackTekEntry.value(QStringLiteral("category")).toString();
            s.blackTekRawEntry = blackTekEntry.value(QStringLiteral("rawEntry")).toString();
            s.blackTekPositionStart = blackTekEntry.value(QStringLiteral("positionStart")).toInt();
            s.blackTekPositionEnd = blackTekEntry.value(QStringLiteral("positionEnd")).toInt();
        }
        if (s.entity == -2 && !s.blackTekEntry.isEmpty()) {
            // A BlackTek creature is independent of any OTBM tile rendered
            // underneath it. Move only the parsed spawn entry.
            snap.push_back(std::move(s));
            continue;
        }
        if (s.entity == -2) {
            // Pixel-perfect picking and right-click creature targeting share
            // -2. If there is no TOML entry, keep the authored tile creature
            // on the existing native-creature move path.
            if (!t || t->creature_name.isEmpty()) continue;
            s.entity = -1;
        }
        if (!t) {
            if (s.entity != -2 && !m_selectionController.wholeStack()) continue;
            snap.push_back(std::move(s));
            continue;
        }
        if (m_selectionController.wholeStack()) {
            s.items.assign(t->items.begin(), t->items.end());
            s.creature = t->creature_name;
            s.monsterChoices = m_otbm->monsterChoicesAt(t->x,t->y,t->z);
            for (auto &choice : s.monsterChoices) choice.direction = (choice.direction + quarterTurns + 4) % 4;
            s.spawntime = t->creature_spawntime;
            s.npc = t->creature_is_npc;
            s.direction = static_cast<uint8_t>((t->creature_direction + quarterTurns + 4) % 4);
            s.spawnRadius = t->spawn_radius;
            s.npcRadius = m_otbm->npcSpawnRadiusAt(t->x,t->y,t->z);
            if (m_selectionController.wholeStack()) s.zoneIds = t->zone_ids;
        } else if (s.entity >= 0) {
            if (size_t(s.entity) < t->items.size()) s.items.push_back(t->items[size_t(s.entity)]);
        } else if (s.entity == -3) {
            s.spawnRadius = t->spawn_radius;
            s.npcRadius = m_otbm->npcSpawnRadiusAt(t->x,t->y,t->z);
        } else if (!t->creature_name.isEmpty()) {

            s.creature = t->creature_name;
            s.monsterChoices = m_otbm->monsterChoicesAt(t->x,t->y,t->z);
            for (auto &choice : s.monsterChoices) choice.direction = (choice.direction + quarterTurns + 4) % 4;
            s.spawntime = t->creature_spawntime;
            s.npc = t->creature_is_npc;
            s.direction = static_cast<uint8_t>((t->creature_direction + quarterTurns + 4) % 4);
        } else if (!t->items.empty()) {
            s.items.push_back(t->items.back());
        } else if (t->spawn_radius > 0) {
            s.spawnRadius = t->spawn_radius;
            s.npcRadius = m_otbm->npcSpawnRadiusAt(t->x,t->y,t->z);
            if (m_selectionController.wholeStack()) s.zoneIds = t->zone_ids;
        }
        if (s.items.empty() && s.creature.isEmpty() && s.spawnRadius == 0 && s.npcRadius == 0
            && s.zoneIds.empty() && s.blackTekEntry.isEmpty()) continue;
        snap.push_back(std::move(s));
    }
    if (snap.empty()) return;
    for (const Snap &s : snap) {
        const int targetX = s.nx;
        const int targetY = s.ny;
        const int targetZ = s.z + dz;
        if (targetX < 0 || targetX > 65535
            || targetY < 0 || targetY > 65535
            || targetZ < 0 || targetZ > 15) return;
    }

    QVariantList blackTekOperations;
    struct DefaultZoneMove { int x, y, z; bool npc; QString entry; };
    QVector<DefaultZoneMove> defaultZoneMoves;
    QSet<quint64> selectedBlackTekPositions;
    QSet<quint64> occupiedBlackTekPositions;
    if (m_otbm->blackTekSpawnMode()) {
        for (const Snap &s : snap)
            if (!s.blackTekEntry.isEmpty())
                selectedBlackTekPositions.insert(selKey(s.x, s.y, s.z));
        for (const auto &spawn : m_otbm->blackTekCreatureSpawns())
            occupiedBlackTekPositions.insert(selKey(spawn.x, spawn.y, spawn.z));
    }
    for (const Snap &s : snap) {
        if (s.blackTekEntry.isEmpty()) continue;
        const int targetX = s.nx, targetY = s.ny, targetZ = s.z + dz;
        const quint64 targetKey = selKey(targetX, targetY, targetZ);
        if (occupiedBlackTekPositions.contains(targetKey)
            && !selectedBlackTekPositions.contains(targetKey)) {
            emit operationWarning(QStringLiteral("A BlackTek creature already occupies the move destination."));
            return;
        }
        const QString sourcePath = s.blackTekEntry.value(QStringLiteral("sourceFile")).toString();
        const QString sourceZone = s.blackTekEntry.value(QStringLiteral("zoneName")).toString();
        const QString sourceCategory = s.blackTekCategory;
        QVariantMap remove = s.blackTekEntry;
        remove.insert(QStringLiteral("action"), QStringLiteral("remove"));
        blackTekOperations.append(remove);

        const QVariantMap destination = m_otbm->blackTekSpawnZoneAt(
            targetX, targetY, targetZ, sourceCategory == QStringLiteral("npcs"));
        QString targetCategory;
        if (!destination.isEmpty()) {
            const QString type = destination.value(QStringLiteral("type")).toString();
            if (sourceCategory == QStringLiteral("npcs") && type == QStringLiteral("npc"))
                targetCategory = QStringLiteral("npcs");
            else if (sourceCategory == QStringLiteral("monsters") && type == QStringLiteral("monster"))
                targetCategory = QStringLiteral("monsters");
            else if (sourceCategory == QStringLiteral("minions") && type == QStringLiteral("boss"))
                targetCategory = QStringLiteral("minions");
            else if (sourceCategory == QStringLiteral("master") && type == QStringLiteral("boss")) {
                const auto creatures = m_otbm->blackTekZoneGeometry(
                    destination.value(QStringLiteral("sourceFile")).toString(),
                    destination.value(QStringLiteral("name")).toString())
                    .value(QStringLiteral("creatures")).toList();
                const bool occupiedMaster = std::any_of(creatures.cbegin(), creatures.cend(), [&](const QVariant &value) {
                    const QVariantMap creature = value.toMap();
                    if (creature.value(QStringLiteral("category")).toString() != QStringLiteral("master")) return false;
                    const int x = creature.value(QStringLiteral("x")).toInt();
                    const int y = creature.value(QStringLiteral("y")).toInt();
                    const int z = creature.value(QStringLiteral("z")).toInt();
                    return !selectedBlackTekPositions.contains(selKey(x, y, z));
                });
                if (!occupiedMaster) targetCategory = QStringLiteral("master");
            }
        }

        if (targetCategory.isEmpty()) {
            if (sourceCategory != QStringLiteral("monsters") && sourceCategory != QStringLiteral("npcs")) {
                emit operationWarning(QStringLiteral("Cannot move this BlackTek creature outside a compatible spawn zone."));
                return;
            }
            const int originX = std::max(0, targetX - 1);
            const int originY = std::max(0, targetY - 1);
            const QString entry = creatureEntryAtPosition(s.blackTekRawEntry,
                s.blackTekPositionStart, s.blackTekPositionEnd,
                targetX - originX, targetY - originY, 0);
            if (entry.isEmpty()) {
                emit operationWarning(QStringLiteral("Could not preserve the moved BlackTek creature entry."));
                return;
            }
            defaultZoneMoves.append({targetX, targetY, targetZ,
                sourceCategory == QStringLiteral("npcs"), entry});
            continue;
        }

        const QString targetPath = destination.value(QStringLiteral("sourceFile")).toString();
        const QString targetZone = destination.value(QStringLiteral("name")).toString();
        const QVariantMap geometry = m_otbm->blackTekZoneGeometry(targetPath, targetZone);
        if (geometry.isEmpty() || geometry.value(QStringLiteral("playerRelative")).toBool()) {
            emit operationWarning(QStringLiteral("Cannot move into an unavailable or player-relative BlackTek zone."));
            return;
        }
        const int originX = geometry.value(QStringLiteral("originX")).toInt();
        const int originY = geometry.value(QStringLiteral("originY")).toInt();
        const int originZ = geometry.value(QStringLiteral("originZ")).toInt();
        if (targetX < originX || targetY < originY || targetZ < originZ) {
            emit operationWarning(QStringLiteral("The move destination is before the BlackTek zone origin."));
            return;
        }
        const QString entry = creatureEntryAtPosition(s.blackTekRawEntry,
            s.blackTekPositionStart, s.blackTekPositionEnd,
            targetX - originX, targetY - originY, targetZ - originZ);
        if (entry.isEmpty()) {
            emit operationWarning(QStringLiteral("Could not preserve the moved BlackTek creature entry."));
            return;
        }
        if (targetCategory == QStringLiteral("master")) {
            const bool movingMasterAlready = std::any_of(snap.cbegin(), snap.cend(), [&](const Snap &candidate) {
                return candidate.blackTekCategory == QStringLiteral("master")
                    && candidate.blackTekEntry.value(QStringLiteral("sourceFile")).toString() == targetPath
                    && candidate.blackTekEntry.value(QStringLiteral("zoneName")).toString() == targetZone;
            });
            if (movingMasterAlready) {
                emit operationWarning(QStringLiteral("A BlackTek boss zone can have only one master."));
                return;
            }
        }
        QVariantMap insert{{QStringLiteral("sourceFile"), targetPath},
                           {QStringLiteral("zoneName"), targetZone},
                           {QStringLiteral("category"), targetCategory},
                           {QStringLiteral("action"), QStringLiteral("insert")},
                           {QStringLiteral("entry"), entry}};
        blackTekOperations.append(insert);
    }

    QSet<quint64> newSel;
    if (quarterTurns != 0) {
        const QRect bounds(QPoint(minX, minY), QPoint(maxX, maxY));
        for (quint64 key : m_selectionController.selected()) {
            const QPoint target = rotatedSelectionPosition(QPoint(selX(key), selY(key)), bounds, quarterTurns);
            if (target.x() < 0 || target.x() > 65535 || target.y() < 0 || target.y() > 65535) return;
            newSel.insert(selKey(target.x(), target.y(), selZ(key)));
        }
    }

    beginEditBatch();
    const bool savedBulk = m_brushController.bulkEdit();
    const bool savedFx = m_placeEffect;
    m_brushController.setBulkEdit(true);
    m_placeEffect = false;
    m_otbm->beginUndoGroup();

    if (!blackTekOperations.isEmpty()) {
        const QString error = m_otbm->editBlackTekCreatureEntries(blackTekOperations);
        if (!error.isEmpty()) {
            m_otbm->cancelUndoGroup();
            m_brushController.setBulkEdit(savedBulk);
            m_placeEffect = savedFx;
            endEditBatch();
            emit operationWarning(error);
            return;
        }
    }
    if (!defaultZoneMoves.isEmpty()) {
        const QString path = QDir(m_otbm->blackTekZoneFolder()).filePath(QStringLiteral("zones.toml"));
        QString staged = m_otbm->blackTekZoneFileText(path);
        for (const DefaultZoneMove &move : defaultZoneMoves) {
            staged = appendDefaultBlackTekSpawnZone(staged, move.x, move.y, move.z,
                                                    move.npc, move.entry);
            if (staged.isEmpty()) {
                m_otbm->cancelUndoGroup();
                m_brushController.setBulkEdit(savedBulk);
                m_placeEffect = savedFx;
                endEditBatch();
                emit operationWarning(QStringLiteral("Could not create a default BlackTek spawn zone for the moved creature."));
                return;
            }
        }
        const auto parsed = BlackTekZones::parse(path, staged.toUtf8());
        if (!parsed.errors.isEmpty()) {
            m_otbm->cancelUndoGroup();
            m_brushController.setBulkEdit(savedBulk);
            m_placeEffect = savedFx;
            endEditBatch();
            emit operationWarning(parsed.errors.join(QLatin1Char('\n')));
            return;
        }
        const QString error = m_otbm->editBlackTekZoneFile(path, staged);
        if (!error.isEmpty()) {
            m_otbm->cancelUndoGroup();
            m_brushController.setBulkEdit(savedBulk);
            m_placeEffect = savedFx;
            endEditBatch();
            emit operationWarning(error);
            return;
        }
    }

    bool movedSpawn = false;
    for (const Snap &s : snap) {
        if (s.entity >= 0) m_otbm->removeItemAt(s.x, s.y, s.z, s.entity);
        else for (size_t i = 0; i < s.items.size(); ++i) m_otbm->removeTopItem(s.x, s.y, s.z);
        if (!s.creature.isEmpty()) m_otbm->clearCreatureAt(s.x, s.y, s.z);
        if (!s.zoneIds.empty()) m_otbm->setTileZoneIds(s.x, s.y, s.z, TileZoneIds());
        if (s.npcRadius > 0) m_otbm->setNpcSpawnAt(s.x,s.y,s.z,0);
        if (s.spawnRadius > 0) { m_otbm->clearSpawnAt(s.x, s.y, s.z); movedSpawn = true; }
        onTileEdited(s.x, s.y, s.z);
    }
    for (Snap &s : snap) {
        const int nx = s.nx, ny = s.ny;
        const int nz = s.z + dz;

        for (OtbmMapItem it : s.items) {
            if (quarterTurns != 0) {
                const auto *store = m_brushController.store();
                int rotated = it.server_id;
                if (store && store->isManagedBorderItem(it.server_id))
                    rotated = store->rotatedSelectionBorderItem(it.server_id, quarterTurns);
                else if (store && store->isWallBrushItem(it.server_id))
                    rotated = store->rotatedSelectionWallItem(it.server_id, quarterTurns);
                if (rotated == it.server_id)
                    rotated = rotatedPathItemId(it.server_id, quarterTurns);
                if (rotated > 0 && rotated <= 65535
                    && (!m_otb || m_otb->rowForServerId(rotated) >= 0))
                    it.server_id = static_cast<uint16_t>(rotated);
            }
            placeItemOnFloor(nx, ny, nz, it);
            if (s.entity >= 0) {
                if (const auto *tile = m_otbm->tileAt(nx, ny, nz)) {
                    for (int index = int(tile->items.size()) - 1; index >= 0; --index)
                        if (tile->items[size_t(index)].server_id == it.server_id) {
                            s.targetEntity = index;
                            break;
                        }
                }
            }
        }
        if (!s.creature.isEmpty()) {
            m_otbm->setCreatureAt(nx, ny, nz, s.creature, s.spawntime, s.npc, s.direction);
            if (!s.npc) m_otbm->setMonsterChoicesAt(nx,ny,nz,s.monsterChoices);
            onTileEdited(nx, ny, nz);
        }
        if (!s.zoneIds.empty() && m_otbm->setTileZoneIds(nx, ny, nz, s.zoneIds)) onTileEdited(nx, ny, nz);
        if (s.npcRadius > 0) m_otbm->setNpcSpawnAt(nx,ny,nz,s.npcRadius);
        if (s.spawnRadius > 0) {
            m_otbm->setSpawnAt(nx, ny, nz, s.spawnRadius);
            onTileEdited(nx, ny, nz);
        }
        newSel.insert(selKey(nx, ny, nz));
    }
    if (movedSpawn) invalidateSpawnIndex();

    if (quarterTurns != 0 && m_brushController.store()) {
        auto *store = m_brushController.store();
        for (const Snap &s : snap) {
            const int z = s.z + dz;
            for (const OtbmMapItem &original : s.items) {
                const QString brush = store->wallBrushForServerId(original.server_id);
                if (brush.isEmpty()) continue;
                const auto hasWall = [&](int x, int y) {
                    const auto *tile = m_otbm->tileAt(x, y, z);
                    if (!tile) return false;
                    for (const auto &item : tile->items)
                        if (store->wallBrushForServerId(item.server_id) == brush) return true;
                    return false;
                };
                int target = store->computeWallItem(brush, hasWall(s.nx, s.ny - 1),
                    hasWall(s.nx - 1, s.ny), hasWall(s.nx + 1, s.ny), hasWall(s.nx, s.ny + 1));
                if (target <= 0 || target > 65535 || (m_otb && m_otb->rowForServerId(target) < 0)) continue;
                const auto *tile = m_otbm->tileAt(s.nx, s.ny, z);
                if (!tile) continue;
                for (size_t i = 0; i < tile->items.size(); ++i) {
                    const int current = tile->items[i].server_id;
                    if (store->wallBrushForServerId(current) != brush) continue;
                    int replacement = target;
                    if (store->isDoorItem(current)) {
                        replacement = store->doorBrushItem(target, current);
                        if (replacement <= 0) continue;
                    }
                    ensureItemSprites(replacement);
                    if (m_otbm->setItemServerIdAt(s.nx, s.ny, z, static_cast<int>(i),
                        static_cast<uint16_t>(replacement))) onTileEdited(s.nx, s.ny, z);
                }
            }
        }
    }

    m_otbm->endUndoGroup();
    m_brushController.setBulkEdit(savedBulk);
    m_placeEffect = savedFx;
    endEditBatch();
    m_selectionController.entities.clear();
    for (const Snap &s : snap) {
        if (s.entity == -1) continue;
        int entity = s.entity;
        if (entity >= 0) {
            entity = s.targetEntity;
        }
        m_selectionController.entities.insert(selKey(s.nx, s.ny, s.z + dz), entity);
    }
    m_selectionController.selected() = newSel;
    notifySelectionChanged();
    refreshAfterEdit(0);
}

void MapView::borderizeSelection()
{
    if (!m_otbm || !m_brushController.store() || !m_brushController.store()->hasData() || m_selectionController.selected().isEmpty()) return;

    std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
    beginEditBatch();
    const bool savedBulk = m_brushController.bulkEdit();
    const bool savedAuto = m_brushController.automagic();
    m_brushController.setBulkEdit(true);
    m_brushController.automagic() = true;
    m_otbm->beginUndoGroup();
    for (quint64 key : m_selectionController.selected()) {

        if (selZ(key) != m_navigationController.floor()) continue;
        recomputeBordersAt(selX(key), selY(key));
    }
    m_otbm->endUndoGroup();
    m_brushController.automagic() = savedAuto;
    m_brushController.setBulkEdit(savedBulk);
    endEditBatch();
    refreshAfterEdit(0);
}

void MapView::borderizeMap()
{
    if (!m_otbm || !m_brushController.store() || !m_brushController.store()->hasData()) return;

    std::vector<std::pair<int, int>> positions;
    positions.reserve(m_otbm->tiles().size());
    for (const OtbmTile &t : m_otbm->tiles())
        if (t.z == m_navigationController.floor()) positions.push_back({ t.x, t.y });

    std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
    beginEditBatch();
    const bool savedBulk = m_brushController.bulkEdit();
    const bool savedAuto = m_brushController.automagic();
    m_brushController.setBulkEdit(true);
    m_brushController.automagic() = true;
    m_otbm->beginUndoGroup();
    for (const auto &p : positions) recomputeBordersAt(p.first, p.second);
    m_otbm->endUndoGroup();
    m_brushController.automagic() = savedAuto;
    m_brushController.setBulkEdit(savedBulk);
    endEditBatch();
    refreshAfterEdit(0);
}

void MapView::randomizeMap()
{
    if (!m_otbm || !m_brushController.store() || !m_brushController.store()->hasData()) return;

    std::vector<std::pair<int, int>> positions;
    positions.reserve(m_otbm->tiles().size());
    for (const OtbmTile &t : m_otbm->tiles())
        if (t.z == m_navigationController.floor()) positions.push_back({ t.x, t.y });

    std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
    beginEditBatch();
    const bool savedBulk = m_brushController.bulkEdit();
    const bool savedFx = m_placeEffect;
    m_brushController.setBulkEdit(true);
    m_placeEffect = false;
    m_otbm->beginUndoGroup();
    for (const auto &p : positions) {
        const QString bn = groundBrushNameAt(p.first, p.second);
        if (bn.isEmpty()) continue;
        const int id = m_brushController.store()->pickGroundItem(bn);
        if (id > 0) placeItemAt(p.first, p.second, id);
    }
    m_otbm->endUndoGroup();
    m_placeEffect = savedFx;
    m_brushController.setBulkEdit(savedBulk);
    endEditBatch();
    refreshAfterEdit(0);
}

int MapView::replaceItemsOnMap(int fromId, int toId)
{
    if (!m_otbm || fromId <= 0 || toId <= 0 || fromId == toId) return 0;
    int n;
    {
        std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
        ensureItemSprites(static_cast<uint16_t>(toId));
        n = m_otbm->replaceItemsOnMap(static_cast<uint16_t>(fromId), static_cast<uint16_t>(toId));
        if (n > 0) refreshUndoRedoTilesLocked();
    }
    if (n > 0) refreshAfterEdit(0);
    return n;
}

int MapView::removeItemsOnMap(int serverId)
{
    if (!m_otbm || serverId <= 0) return 0;
    int n;
    {
        std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
        n = m_otbm->removeItemsOnMap(static_cast<uint16_t>(serverId));
        if (n > 0) refreshUndoRedoTilesLocked();
    }
    if (n > 0) refreshAfterEdit(0);
    return n;
}

void MapView::centerOnPosition(int x, int y, int z)
{

    m_navigationController.previousCenterX() = static_cast<int>(m_navigationController.originX() + width() / (2.0 * std::max(1, m_navigationController.tileSize())));
    m_navigationController.previousCenterY() = static_cast<int>(m_navigationController.originY() + height() / (2.0 * std::max(1, m_navigationController.tileSize())));
    m_navigationController.previousCenterZ() = m_navigationController.floor();
    m_navigationController.previousCenterValid() = true;

    if (z >= 0 && z <= 15 && z != m_navigationController.floor()) setFloor(z);
    const double ts = std::max(1, m_navigationController.tileSize());
    m_navigationController.originX() = x - width() / (2.0 * ts);
    m_navigationController.originY() = y - height() / (2.0 * ts);
    emit contentUpdated(); update();
}

bool MapView::goToPreviousPosition()
{
    if (!m_navigationController.previousCenterValid()) return false;
    const int x = m_navigationController.previousCenterX(), y = m_navigationController.previousCenterY(), z = m_navigationController.previousCenterZ();
    centerOnPosition(x, y, z);
    return true;
}

bool MapView::jumpToItemOnMap(int serverId)
{
    if (!m_otbm || serverId <= 0) return false;
    const QVariantMap pos = m_otbm->findFirstItemOnMap(serverId);
    if (pos.isEmpty()) return false;
    centerOnPosition(pos.value(QStringLiteral("x")).toInt(),
                     pos.value(QStringLiteral("y")).toInt(),
                     pos.value(QStringLiteral("z")).toInt());
    return true;
}

void MapView::randomizeSelection()
{
    if (!m_otbm || !m_brushController.store() || !m_brushController.store()->hasData() || m_selectionController.selected().isEmpty()) return;

    std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
    beginEditBatch();
    const bool savedBulk = m_brushController.bulkEdit();
    const bool savedFx = m_placeEffect;
    m_brushController.setBulkEdit(true);
    m_placeEffect = false;
    m_otbm->beginUndoGroup();
    for (quint64 key : m_selectionController.selected()) {

        if (selZ(key) != m_navigationController.floor()) continue;
        const int x = selX(key), y = selY(key);

        const QString bn = groundBrushNameAt(x, y);
        if (bn.isEmpty()) continue;
        const int id = m_brushController.store()->pickGroundItem(bn);
        if (id > 0) placeItemAt(x, y, id);
    }
    m_otbm->endUndoGroup();
    m_placeEffect = savedFx;
    m_brushController.setBulkEdit(savedBulk);
    endEditBatch();
    refreshAfterEdit(0);
}

QVariantMap MapView::aiSelectionContext() const
{
    QVariantMap result;
    result.insert(QStringLiteral("valid"), false);
    if (!m_otbm || !m_otbm->isLoaded()) {
        result.insert(QStringLiteral("error"), QStringLiteral("No map is loaded."));
        return result;
    }
    if (!m_brushController.store() || !m_brushController.store()->hasData()) {
        result.insert(QStringLiteral("error"), QStringLiteral("No brush data is loaded."));
        return result;
    }
    if (m_selectionController.selected().isEmpty()) {
        result.insert(QStringLiteral("error"), QStringLiteral("Select an area first."));
        return result;
    }
    if (m_selectionController.selected().size() > 1024) {
        result.insert(QStringLiteral("error"),
                      QStringLiteral("The AI assistant supports at most 1024 selected tiles."));
        return result;
    }

    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int maxX = std::numeric_limits<int>::min();
    int maxY = std::numeric_limits<int>::min();
    int selectedFloor = -1;
    for (quint64 key : m_selectionController.selected()) {
        if (selectedFloor < 0) selectedFloor = selZ(key);
        if (selZ(key) != selectedFloor) {
            result.insert(QStringLiteral("error"),
                          QStringLiteral("Select tiles on one floor only."));
            return result;
        }
        minX = std::min(minX, selX(key));
        minY = std::min(minY, selY(key));
        maxX = std::max(maxX, selX(key));
        maxY = std::max(maxY, selY(key));
    }
    if (selectedFloor != m_navigationController.floor()) {
        result.insert(QStringLiteral("error"),
                      QStringLiteral("The selection must be on the current floor."));
        return result;
    }

    QVariantList tiles;
    tiles.reserve(m_selectionController.selected().size());
    for (quint64 key : m_selectionController.selected()) {
        QVariantMap tile;
        const int x = selX(key), y = selY(key);
        tile.insert(QStringLiteral("x"), x - minX);
        tile.insert(QStringLiteral("y"), y - minY);
        const QString ground = groundBrushNameAt(x, y);
        tile.insert(QStringLiteral("ground"), ground.isEmpty() ? QStringLiteral("empty") : ground);
        tiles.append(tile);
    }

    result.insert(QStringLiteral("valid"), true);
    result.insert(QStringLiteral("originX"), minX);
    result.insert(QStringLiteral("originY"), minY);
    result.insert(QStringLiteral("floor"), selectedFloor);
    result.insert(QStringLiteral("width"), maxX - minX + 1);
    result.insert(QStringLiteral("height"), maxY - minY + 1);
    result.insert(QStringLiteral("selected_tiles"), tiles);
    result.insert(QStringLiteral("available_brushes"),
                  m_brushController.store()->groundBrushNames());
    return result;
}

QVariantMap MapView::createBlackTekZoneFromSelection(const QVariantMap &options)
{
    return createBlackTekZone(options);
}

QVariantMap MapView::createBlackTekZoneFromPositions(const QVariantMap &options,
                                                     const QVariantList &positions)
{
    QSet<quint64> selected;
    selected.reserve(positions.size());
    for (const QVariant &value : positions) {
        const QVariantMap position = value.toMap();
        const int x = position.value(QStringLiteral("x")).toInt();
        const int y = position.value(QStringLiteral("y")).toInt();
        const int z = position.value(QStringLiteral("z")).toInt();
        if (x < 0 || x > 65535 || y < 0 || y > 65535 || z < 0 || z > 255)
            return {{QStringLiteral("error"), QStringLiteral("The brush footprint is outside BlackTek zone coordinate limits.")}};
        selected.insert(selKey(x, y, z));
    }
    if (selected.isEmpty())
        return {{QStringLiteral("error"), QStringLiteral("Paint at least one tile to create a zone.")}};
    return createBlackTekZone(options, &selected);
}

QVariantMap MapView::createBlackTekZone(const QVariantMap &options, const QSet<quint64> *positions)
{
    const auto failure = [](const QString &error) {
        return QVariantMap{{QStringLiteral("error"), error}};
    };
    if (!m_otbm || !m_otbm->isLoaded())
        return failure(QStringLiteral("Load a map before creating a zone."));
    if (!m_otbm->blackTekSpawnMode() || m_otbm->blackTekZoneFolder().isEmpty())
        return failure(QStringLiteral("Link a BlackTek zones folder before creating a zone."));
    const QSet<quint64> &selectedPositions = positions ? *positions : m_selectionController.selected();
    if (selectedPositions.isEmpty())
        return failure(QStringLiteral("Select the tiles for the zone first."));
    std::lock_guard<std::recursive_mutex> dataLock(m_dataMutex);

    const QString name = options.value(QStringLiteral("name")).toString().trimmed();
    const QString type = options.value(QStringLiteral("type")).toString().trimmed().toLower();
    if (name.isEmpty()) return failure(QStringLiteral("Enter a zone name."));
    if (type != QStringLiteral("world") && type != QStringLiteral("monster")
        && type != QStringLiteral("npc") && type != QStringLiteral("boss")
        && type != QStringLiteral("item"))
        return failure(QStringLiteral("Choose a valid zone type."));

    QString quotedName;
    if (!appendTomlString(quotedName, name))
        return failure(QStringLiteral("The zone name cannot contain control characters."));

    struct Point { int x; int y; int z; };
    std::vector<Point> points;
    points.reserve(static_cast<size_t>(selectedPositions.size()));
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int minZ = std::numeric_limits<int>::max();
    for (const quint64 key : selectedPositions) {
        const Point point{selX(key), selY(key), selZ(key)};
        if (point.x < 0 || point.x > 65535 || point.y < 0 || point.y > 65535
            || point.z < 0 || point.z > 255)
            return failure(QStringLiteral("The selection contains coordinates the BlackTek zone format cannot store."));
        points.push_back(point);
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        minZ = std::min(minZ, point.z);
    }
    std::sort(points.begin(), points.end(), [](const Point &left, const Point &right) {
        return std::tie(left.z, left.y, left.x) < std::tie(right.z, right.y, right.x);
    });

    QStringList flags;
    QSet<QString> seenFlags;
    for (const QVariant &value : options.value(QStringLiteral("flags")).toList()) {
        const QString flag = value.toString();
        if (!isValidZoneFlag(flag)) return failure(QStringLiteral("Unknown BlackTek zone flag: %1").arg(flag));
        if (seenFlags.contains(flag)) return failure(QStringLiteral("The zone flag %1 was selected more than once.").arg(flag));
        seenFlags.insert(flag);
        flags.append(flag);
    }

    QString policy = options.value(QStringLiteral("policy")).toString().trimmed().toLower();
    QString trigger = options.value(QStringLiteral("trigger")).toString().trimmed();
    const bool isCreatureZone = type == QStringLiteral("monster") || type == QStringLiteral("npc")
                             || type == QStringLiteral("boss");
    if (isCreatureZone && policy.isEmpty()) policy = QStringLiteral("fixed");
    if (isCreatureZone && policy != QStringLiteral("fixed") && policy != QStringLiteral("relative")
        && policy != QStringLiteral("staged") && policy != QStringLiteral("triggered"))
        return failure(QStringLiteral("Choose a valid spawn policy."));
    if (type == QStringLiteral("npc")
        && policy != QStringLiteral("fixed") && policy != QStringLiteral("relative"))
        return failure(QStringLiteral("NPC zones support fixed or relative policies."));
    if (policy == QStringLiteral("triggered") && !isValidZoneTrigger(trigger))
        return failure(QStringLiteral("Choose a valid trigger for this triggered spawn zone."));
    if (!trigger.isEmpty() && !isValidZoneTrigger(trigger))
        return failure(QStringLiteral("Choose a valid BlackTek zone trigger."));

    QString text = m_otbm->blackTekZoneFileText(
        QDir(m_otbm->blackTekZoneFolder()).filePath(QStringLiteral("zones.toml")));
    const QString sourceFile = QDir(m_otbm->blackTekZoneFolder()).filePath(QStringLiteral("zones.toml"));
    const auto existing = BlackTekZones::parse(sourceFile, text.toUtf8());
    if (!existing.errors.isEmpty())
        return failure(QStringLiteral("Cannot add a zone because zones.toml is malformed: %1")
                       .arg(existing.errors.join(QLatin1Char('\n'))));
    for (const auto &zone : existing.zones)
        if (zone.name.compare(name, Qt::CaseInsensitive) == 0)
            return failure(QStringLiteral("A zone named %1 already exists in zones.toml.").arg(name));
    for (const QVariant &entryValue : m_otbm->blackTekZoneEntries()) {
        const QVariantMap entry = entryValue.toMap();
        if (entry.value(QStringLiteral("name")).toString().compare(name, Qt::CaseInsensitive) == 0)
            return failure(QStringLiteral("A zone named %1 already exists in %2.")
                           .arg(name, entry.value(QStringLiteral("sourceFile")).toString()));
    }

    QString zone;
    zone += QLatin1Char('[') + quotedName + QStringLiteral("]\n");
    zone += QStringLiteral("positions = [");
    for (size_t index = 0; index < points.size(); ++index) {
        if (index) zone += QStringLiteral(", ");
        const auto &point = points[index];
        zone += QStringLiteral("\"%1:%2:%3\"").arg(point.x).arg(point.y).arg(point.z);
    }
    zone += QStringLiteral("]\n");
    if (!flags.isEmpty()) {
        zone += QStringLiteral("flags = [");
        for (qsizetype index = 0; index < flags.size(); ++index) {
            if (index) zone += QStringLiteral(", ");
            zone += QLatin1Char('"') + flags[index] + QLatin1Char('"');
        }
        zone += QStringLiteral("]\n");
    }
    if (type != QStringLiteral("world")) {
        zone += QStringLiteral("spawn_type = \"%1\"\n").arg(type);
        if (type != QStringLiteral("item")) zone += QStringLiteral("policy = \"%1\"\n").arg(policy);
        if (!trigger.isEmpty()) zone += QStringLiteral("trigger = \"%1\"\n").arg(trigger);
    }

    if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n'))) text += QLatin1Char('\n');
    if (!text.isEmpty()) text += QLatin1Char('\n');
    text += zone;
    const QString error = m_otbm->editBlackTekZoneFile(sourceFile, text);
    if (!error.isEmpty()) return failure(error);
    return {{QStringLiteral("name"), name}, {QStringLiteral("sourceFile"), sourceFile},
            {QStringLiteral("x"), minX}, {QStringLiteral("y"), minY}, {QStringLiteral("z"), minZ}};
}

QVariantMap MapView::applyAiGroundPlan(const QVariantMap &plan,
                                       const QVariantMap &context)
{
    QVariantMap result;
    result.insert(QStringLiteral("success"), false);
    if (!m_otbm || !m_brushController.store()) {
        result.insert(QStringLiteral("error"), QStringLiteral("Map brushes are not available."));
        return result;
    }
    if (!context.value(QStringLiteral("valid")).toBool()) {
        result.insert(QStringLiteral("error"), QStringLiteral("The generation context is invalid."));
        return result;
    }

    const int originX = context.value(QStringLiteral("originX")).toInt();
    const int originY = context.value(QStringLiteral("originY")).toInt();
    const int floor = context.value(QStringLiteral("floor")).toInt();
    const QVariantList operations = plan.value(QStringLiteral("operations")).toList();
    if (operations.isEmpty()) {
        result.insert(QStringLiteral("error"), QStringLiteral("The AI plan contains no operations."));
        return result;
    }
    if (operations.size() > 1024) {
        result.insert(QStringLiteral("error"), QStringLiteral("The AI plan is too large."));
        return result;
    }

    struct Paint { int x; int y; QString brush; };
    std::vector<Paint> paints;
    paints.reserve(static_cast<size_t>(operations.size()));
    QSet<quint64> changed;
    for (const QVariant &operationValue : operations) {
        const QVariantMap operation = operationValue.toMap();
        const int x = originX + operation.value(QStringLiteral("x")).toInt();
        const int y = originY + operation.value(QStringLiteral("y")).toInt();
        const QString brush = operation.value(QStringLiteral("brush")).toString();
        if (!m_selectionController.selected().contains(selKey(x, y, floor))) {
            result.insert(QStringLiteral("error"),
                          QStringLiteral("The AI plan tried to edit a tile outside the selection."));
            return result;
        }
        if (!m_brushController.store()->isGroundBrush(brush)) {
            result.insert(QStringLiteral("error"),
                          QStringLiteral("Unknown ground brush: %1").arg(brush));
            return result;
        }
        paints.push_back({x, y, brush});
        changed.insert(selKey(x, y, floor));
    }

    std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
    beginEditBatch();
    const bool savedBulk = m_brushController.bulkEdit();
    const bool savedAuto = m_brushController.automagic();
    const bool savedFx = m_placeEffect;
    m_brushController.setBulkEdit(true);
    m_brushController.automagic() = false;
    m_placeEffect = false;
    m_otbm->beginUndoGroup();
    for (const Paint &paint : paints) {
        const int id = m_brushController.store()->pickGroundItem(paint.brush);
        if (id > 0) placeItemOnFloor(paint.x, paint.y, floor, id);
    }
    m_brushController.automagic() = true;
    for (quint64 key : changed) {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                recomputeBordersAt(selX(key) + dx, selY(key) + dy);
    }
    m_otbm->endUndoGroup();
    m_placeEffect = savedFx;
    m_brushController.automagic() = savedAuto;
    m_brushController.setBulkEdit(savedBulk);
    endEditBatch();
    refreshAfterEdit(0);

    result.insert(QStringLiteral("success"), true);
    result.insert(QStringLiteral("count"), static_cast<int>(paints.size()));
    return result;
}

int MapView::countItemOnSelection(int serverId) const
{
    if (!m_otbm || m_selectionController.selected().isEmpty() || serverId <= 0) return 0;
    int n = 0;
    for (quint64 key : m_selectionController.selected()) {

        n += m_otbm->countItemsOnTile(selX(key), selY(key), selZ(key), serverId);
    }
    return n;
}

int MapView::removeItemOnSelection(int serverId)
{
    if (!m_otbm || m_selectionController.selected().isEmpty() || serverId <= 0) return 0;

    std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
    beginEditBatch();
    m_otbm->beginUndoGroup();
    const std::vector<uint16_t> ids{ static_cast<uint16_t>(serverId) };
    int n = 0;
    for (quint64 key : m_selectionController.selected()) {
        const int x = selX(key), y = selY(key), z = selZ(key);
        const int c = m_otbm->removeItemsById(x, y, z, ids, /*deep=*/true);
        if (c > 0) { n += c; onTileEdited(x, y, z); }
    }
    m_otbm->endUndoGroup();
    endEditBatch();
    if (n > 0) refreshAfterEdit(0);
    return n;
}

int MapView::replaceItemsOnSelection(int fromId, int toId)
{
    if (!m_otbm || m_selectionController.selected().isEmpty() || fromId <= 0 || toId <= 0 || fromId == toId) return 0;

    std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
    ensureItemSprites(static_cast<uint16_t>(toId));
    beginEditBatch();
    m_otbm->beginUndoGroup();
    int n = 0;
    for (quint64 key : m_selectionController.selected()) {
        const int x = selX(key), y = selY(key), z = selZ(key);
        const int c = m_otbm->replaceItemsById(x, y, z, static_cast<uint16_t>(fromId),
                                               static_cast<uint16_t>(toId));
        if (c > 0) { n += c; onTileEdited(x, y, z); }
    }
    m_otbm->endUndoGroup();
    endEditBatch();
    if (n > 0) refreshAfterEdit(0);
    return n;
}

void MapView::startPasting()
{
    if (!m_otbm || m_selectionController.clipboard().empty()) return;
    {

        std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
        for (const ClipTile &ct : m_selectionController.clipboard())
            for (const OtbmMapItem &ci : ct.items) ensureItemSprites(ci.server_id);
    }
    m_selectionController.pasting() = true;
    setCursor(Qt::CrossCursor);
    emit pastingChanged();
    emit contentUpdated(); update();
}

void MapView::cancelPasting()
{
    if (!m_selectionController.pasting()) return;
    m_selectionController.pasting() = false;
    setCursor((m_brushController.serverId() > 0 || m_editController.activeZone() != 0 || m_editController.eraseMode()) ? Qt::CrossCursor
                                                                        : Qt::ArrowCursor);
    emit pastingChanged();
    emit contentUpdated(); update();
}

void MapView::commitPasteAt(int px, int py)
{
    if (!m_otbm || m_selectionController.clipboard().empty()) return;

    for (const ClipTile &ct : m_selectionController.clipboard()) {
        const int targetZ = m_navigationController.floor() + ct.dz;
        if (targetZ < 0 || targetZ > 15) {
            emit operationWarning(QStringLiteral(
                "Cannot paste: the multi-floor selection would extend outside floors 0-15."));
            return;
        }
    }

    std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
    QHash<QString, QString> blackTekUpdates;
    QSet<quint64> occupiedBlackTekPositions;
    QSet<QPair<QString, QString>> pastedMasters;
    const bool hasBlackTekCreature = std::any_of(
        m_selectionController.clipboard().cbegin(), m_selectionController.clipboard().cend(),
        [](const ClipTile &tile) { return tile.blackTekCreature; });
    if (hasBlackTekCreature && m_otbm->blackTekSpawnMode())
        for (const auto &spawn : m_otbm->blackTekCreatureSpawns())
            occupiedBlackTekPositions.insert(selKey(spawn.x, spawn.y, spawn.z));
    for (const ClipTile &ct : m_selectionController.clipboard()) {
        if (!ct.blackTekCreature) continue;
        const int tx = px + ct.dx, ty = py + ct.dy;
        const int tz = m_navigationController.floor() + ct.dz;
        if (occupiedBlackTekPositions.contains(selKey(tx, ty, tz))) {
            emit operationWarning(QStringLiteral("A BlackTek creature already occupies the paste destination."));
            return;
        }
        const QVariantMap destination = m_otbm->blackTekSpawnZoneAt(
            tx, ty, tz, ct.blackTekCategory == QStringLiteral("npcs"));
        if (destination.isEmpty()) {
            if (ct.blackTekCategory != QStringLiteral("monsters")
                && ct.blackTekCategory != QStringLiteral("npcs")) {
                emit operationWarning(QStringLiteral("Cannot paste this creature outside a compatible BlackTek spawn zone."));
                return;
            }
            const int originX = std::max(0, tx - 1);
            const int originY = std::max(0, ty - 1);
            const QString entry = creatureEntryAtPosition(ct.blackTekRawEntry,
                ct.blackTekPositionStart, ct.blackTekPositionEnd,
                tx - originX, ty - originY, 0);
            if (entry.isEmpty()) {
                emit operationWarning(QStringLiteral("Could not preserve the copied BlackTek creature entry."));
                return;
            }
            const QString path = QDir(m_otbm->blackTekZoneFolder()).filePath(QStringLiteral("zones.toml"));
            const QString text = blackTekUpdates.value(path, m_otbm->blackTekZoneFileText(path));
            const QString updated = appendDefaultBlackTekSpawnZone(text, tx, ty, tz,
                ct.blackTekCategory == QStringLiteral("npcs"), entry);
            if (updated.isEmpty()) {
                emit operationWarning(QStringLiteral("Could not create a default BlackTek spawn zone for the pasted creature."));
                return;
            }
            const auto parsed = BlackTekZones::parse(path, updated.toUtf8());
            if (!parsed.errors.isEmpty()) {
                emit operationWarning(parsed.errors.join(QLatin1Char('\n')));
                return;
            }
            blackTekUpdates.insert(path, updated);
            occupiedBlackTekPositions.insert(selKey(tx, ty, tz));
            continue;
        }
        const QString type = destination.value(QStringLiteral("type")).toString();
        QString category;
        if (ct.blackTekCategory == QStringLiteral("npcs") && type == QStringLiteral("npc"))
            category = QStringLiteral("npcs");
        else if (ct.blackTekCategory == QStringLiteral("monsters") && type == QStringLiteral("monster"))
            category = QStringLiteral("monsters");
        else if (ct.blackTekCategory == QStringLiteral("minions") && type == QStringLiteral("boss"))
            category = QStringLiteral("minions");
        else if (ct.blackTekCategory == QStringLiteral("master") && type == QStringLiteral("boss")) {
            const QVariantList creatures = m_otbm->blackTekZoneGeometry(
                destination.value(QStringLiteral("sourceFile")).toString(),
                destination.value(QStringLiteral("name")).toString())
                .value(QStringLiteral("creatures")).toList();
            const bool hasMaster = std::any_of(creatures.cbegin(), creatures.cend(), [](const QVariant &value) {
                return value.toMap().value(QStringLiteral("category")).toString() == QStringLiteral("master");
            });
            if (!hasMaster) category = QStringLiteral("master");
        }
        if (category.isEmpty()) {
            emit operationWarning(QStringLiteral("Cannot paste this creature into an incompatible BlackTek spawn zone."));
            return;
        }
        const QString path = destination.value(QStringLiteral("sourceFile")).toString();
        const QString zoneName = destination.value(QStringLiteral("name")).toString();
        const QVariantMap geometry = m_otbm->blackTekZoneGeometry(path, zoneName);
        if (geometry.isEmpty()) {
            emit operationWarning(QStringLiteral("The destination BlackTek zone is no longer available."));
            return;
        }
        if (geometry.value(QStringLiteral("playerRelative")).toBool()) {
            emit operationWarning(QStringLiteral("Cannot paste into a player-relative BlackTek spawn zone."));
            return;
        }
        const int originX = geometry.value(QStringLiteral("originX")).toInt();
        const int originY = geometry.value(QStringLiteral("originY")).toInt();
        const int originZ = geometry.value(QStringLiteral("originZ")).toInt();
        if (tx < originX || ty < originY || tz < originZ) {
            emit operationWarning(QStringLiteral("The paste destination is before the BlackTek zone origin."));
            return;
        }
        const QString entry = creatureEntryAtPosition(ct.blackTekRawEntry,
            ct.blackTekPositionStart, ct.blackTekPositionEnd,
            tx - originX, ty - originY, tz - originZ);
        if (entry.isEmpty()) {
            emit operationWarning(QStringLiteral("Could not preserve the copied BlackTek creature entry."));
            return;
        }
        const bool master = category == QStringLiteral("master");
        const auto zoneKey = qMakePair(path, zoneName);
        if (master && pastedMasters.contains(zoneKey)) {
            emit operationWarning(QStringLiteral("A BlackTek boss zone can have only one master."));
            return;
        }
        if (master) pastedMasters.insert(zoneKey);
        const QString text = blackTekUpdates.value(path, m_otbm->blackTekZoneFileText(path));
        const QString updated = m_otbm->replaceBlackTekZoneField(text, zoneName, category, entry, !master);
        if (updated.isEmpty()) {
            emit operationWarning(QStringLiteral("Could not prepare the BlackTek creature paste."));
            return;
        }
        const auto parsed = BlackTekZones::parse(path, updated.toUtf8());
        if (!parsed.errors.isEmpty()) {
            emit operationWarning(parsed.errors.join(QLatin1Char('\n')));
            return;
        }
        blackTekUpdates.insert(path, updated);
    }
    QVariantList zoneEdits;
    for (auto it = blackTekUpdates.cbegin(); it != blackTekUpdates.cend(); ++it) {
        zoneEdits.append(QVariantMap{{QStringLiteral("sourceFile"), it.key()},
                                     {QStringLiteral("action"), QStringLiteral("file")},
                                     {QStringLiteral("text"), it.value()}});
    }
    beginEditBatch();
    const bool savedBulk = m_brushController.bulkEdit();
    const bool savedFx = m_placeEffect;
    m_brushController.setBulkEdit(true);
    m_placeEffect = false;
    m_otbm->beginUndoGroup();
    if (!zoneEdits.isEmpty()) {
        const QString error = m_otbm->editBlackTekCreatureEntries(zoneEdits);
        if (!error.isEmpty()) {
            m_otbm->cancelUndoGroup();
            m_brushController.setBulkEdit(savedBulk);
            m_placeEffect = savedFx;
            endEditBatch();
            emit operationWarning(error);
            return;
        }
    }
    bool pastedSpawn = false;
    for (const ClipTile &ct : m_selectionController.clipboard()) {
        const int tx = px + ct.dx, ty = py + ct.dy;

        const int tz = m_navigationController.floor() + ct.dz;

        for (const OtbmMapItem &ci : ct.items)
            placeItemOnFloor(tx, ty, tz, ci);
        if (!ct.blackTekCreature && !ct.creature.isEmpty()) {
            m_otbm->setCreatureAt(tx, ty, tz, ct.creature, ct.spawntime, ct.npc, ct.direction);
            if (!ct.npc) m_otbm->setMonsterChoicesAt(tx,ty,tz,ct.monsterChoices);
            onTileEdited(tx, ty, tz);
        }
        if (!ct.zoneIds.empty() && m_otbm->setTileZoneIds(tx, ty, tz, ct.zoneIds)) onTileEdited(tx, ty, tz);
        if (ct.npcRadius > 0) m_otbm->setNpcSpawnAt(tx,ty,tz,ct.npcRadius);
        if (ct.spawnRadius > 0) {
            m_otbm->setSpawnAt(tx, ty, tz, ct.spawnRadius);
            onTileEdited(tx, ty, tz);
            pastedSpawn = true;
        }
    }
    if (pastedSpawn) invalidateSpawnIndex();
    m_otbm->endUndoGroup();
    m_brushController.setBulkEdit(savedBulk);
    m_placeEffect = savedFx;
    endEditBatch();
    refreshAfterEdit(0);
}
