
#include "mapview.h"
#include "mapview_p.h"
#include "loadprofile.h"

#include <QPainter>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QWheelEvent>
#include <QCursor>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QTimer>
#include <QGuiApplication>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

void MapView::buildStaticIndex()
{
    auto &tileIndex = m_chunkStore.tiles();
    tileIndex.clear();
    m_chunkStore.bounds() = {};
    m_mapTileBounds = {};
    m_chunkStore.dirtyBounds().clear();
    m_blackTekSpawnChunks.clear();
    m_blackTekSpawnOnlyChunks.clear();
    MapSpawnIndexService::FloorCenters spawnCenters;
    m_chunkStore.indexedTileCount() = 0;
    if (!m_otbm || !m_otbm->isLoaded()) {
        indexBlackTekZoneGeometry();
        return;
    }

    for (const OtbmTile &tile : m_otbm->tiles()) {
        m_chunkStore.bounds()[tile.z].include(tile.x, tile.y);
        const int cx = floorDiv(tile.x, kChunkTiles);
        const int cy = floorDiv(tile.y, kChunkTiles);
        tileIndex[tile.z][chunkKey(cx, cy)].push_back(&tile);
        auto &floorSpawns = spawnCenters[tile.z];
        if (tile.spawn_radius > 0)
            floorSpawns.push_back({tile.x, tile.y, tile.spawn_radius});
    }
    for (auto floorIt = tileIndex.begin(); floorIt != tileIndex.end();
         ++floorIt) {
        for (auto chunkIt = floorIt->begin(); chunkIt != floorIt->end(); ++chunkIt) {
            auto &tiles = chunkIt.value();
            std::sort(tiles.begin(), tiles.end(),
                      [](const OtbmTile *a, const OtbmTile *b) {
                          return a->y != b->y ? a->y < b->y : a->x < b->x;
                      });
        }
    }
    m_spawnIndex.setPrebuilt(std::move(spawnCenters));
    m_chunkStore.indexedTileCount() = static_cast<qsizetype>(m_otbm->tiles().size());
    m_mapTileBounds = m_chunkStore.bounds();
    indexBlackTekSpawns();
}

QVector<QPair<int, quint64>> MapView::indexBlackTekSpawns()
{
    auto &tileIndex = m_chunkStore.tiles();
    for (auto floor = m_blackTekSpawnOnlyChunks.cbegin();
         floor != m_blackTekSpawnOnlyChunks.cend(); ++floor) {
        auto floorTiles = tileIndex.find(floor.key());
        if (floorTiles == tileIndex.end())
            continue;
        for (quint64 key : floor.value()) {
            const auto chunk = floorTiles->find(key);
            if (chunk != floorTiles->end() && chunk->empty())
                floorTiles->erase(chunk);
        }
    }
    m_blackTekSpawnOnlyChunks.clear();
    auto previousSpawns = std::move(m_blackTekSpawnChunks);
    m_blackTekSpawnChunks.clear();
    QHash<QString, CreatureStore::CreatureType> catalog;
    if (m_creatureStore && m_otbm && m_otbm->isLoaded()
        && m_otbm->blackTekSpawnMode()) {
        catalog.reserve(m_creatureStore->count());
        for (const auto &creature : m_creatureStore->creatureTypes()) {
            const QString key = (creature.isNpc ? QStringLiteral("npc:")
                                                 : QStringLiteral("monster:"))
                              + creature.name.toCaseFolded();
            if (!catalog.contains(key))
                catalog.insert(key, creature);
        }
    }
    if (m_otbm && m_otbm->isLoaded() && m_otbm->blackTekSpawnMode()) {
        for (const auto &spawn : m_otbm->blackTekCreatureSpawns()) {
            const quint64 key = chunkKey(floorDiv(spawn.x, kChunkTiles),
                                         floorDiv(spawn.y, kChunkTiles));
            auto &floor = tileIndex[spawn.z];
            if (!floor.contains(key)) {
                floor.insert(key, {});
                m_blackTekSpawnOnlyChunks[spawn.z].insert(key);
            }

            BlackTekRenderSpawn indexed;
            indexed.x = spawn.x;
            indexed.y = spawn.y;
            indexed.z = spawn.z;
            for (const QString &name : spawn.names) {
                const QString catalogKey = (spawn.isNpc ? QStringLiteral("npc:")
                                                         : QStringLiteral("monster:"))
                                         + name.toCaseFolded();
                const auto creature = catalog.constFind(catalogKey);
                if (creature == catalog.cend())
                    continue;
                indexed.creature = creature.value();
                indexed.hasCreature = true;
                break;
            }
            m_blackTekSpawnChunks[spawn.z][key].append(std::move(indexed));
        }
    }
    for (auto floor = m_blackTekSpawnChunks.begin();
         floor != m_blackTekSpawnChunks.end(); ++floor) {
        for (auto chunk = floor->begin(); chunk != floor->end(); ++chunk) {
            std::sort(chunk->begin(), chunk->end(),
                      [](const auto &left, const auto &right) {
                          return left.y != right.y ? left.y < right.y
                                                   : left.x < right.x;
                      });
        }
    }

    m_chunkStore.bounds() = m_mapTileBounds;
    for (auto floor = m_blackTekSpawnChunks.cbegin(); floor != m_blackTekSpawnChunks.cend(); ++floor)
        for (auto chunk = floor->cbegin(); chunk != floor->cend(); ++chunk)
            for (const auto &spawn : chunk.value())
                m_chunkStore.bounds()[spawn.z].include(spawn.x, spawn.y);

    const auto sameSpawn = [](const BlackTekRenderSpawn &left,
                              const BlackTekRenderSpawn &right) {
        if (left.x != right.x || left.y != right.y || left.z != right.z
            || left.hasCreature != right.hasCreature)
            return false;
        if (!left.hasCreature)
            return true;
        return left.creature.lookType == right.creature.lookType
            && left.creature.lookItem == right.creature.lookItem
            && left.creature.lookHead == right.creature.lookHead
            && left.creature.lookBody == right.creature.lookBody
            && left.creature.lookLegs == right.creature.lookLegs
            && left.creature.lookFeet == right.creature.lookFeet;
    };
    const auto sameChunk = [&sameSpawn](const QVector<BlackTekRenderSpawn> *left,
                                        const QVector<BlackTekRenderSpawn> *right) {
        if (!left || !right)
            return (!left || left->isEmpty()) && (!right || right->isEmpty());
        if (left->size() != right->size())
            return false;
        for (qsizetype i = 0; i < left->size(); ++i)
            if (!sameSpawn(left->at(i), right->at(i)))
                return false;
        return true;
    };
    std::set<std::pair<int, quint64>> affected;
    for (auto floor = previousSpawns.cbegin(); floor != previousSpawns.cend(); ++floor) {
        for (auto chunk = floor->cbegin(); chunk != floor->cend(); ++chunk) {
            const auto nextFloor = m_blackTekSpawnChunks.constFind(floor.key());
            const auto nextChunk = nextFloor != m_blackTekSpawnChunks.cend()
                ? nextFloor->constFind(chunk.key())
                : QHash<quint64, QVector<BlackTekRenderSpawn>>::const_iterator();
            const auto *next = nextFloor != m_blackTekSpawnChunks.cend()
                    && nextChunk != nextFloor->cend() ? &nextChunk.value() : nullptr;
            if (!sameChunk(&chunk.value(), next))
                affected.insert({floor.key(), chunk.key()});
        }
    }
    for (auto floor = m_blackTekSpawnChunks.cbegin(); floor != m_blackTekSpawnChunks.cend(); ++floor) {
        for (auto chunk = floor->cbegin(); chunk != floor->cend(); ++chunk) {
            const auto oldFloor = previousSpawns.constFind(floor.key());
            const auto oldChunk = oldFloor != previousSpawns.cend()
                ? oldFloor->constFind(chunk.key())
                : QHash<quint64, QVector<BlackTekRenderSpawn>>::const_iterator();
            const auto *old = oldFloor != previousSpawns.cend()
                    && oldChunk != oldFloor->cend() ? &oldChunk.value() : nullptr;
            if (!sameChunk(old, &chunk.value()))
                affected.insert({floor.key(), chunk.key()});
        }
    }

    QVector<QPair<int, quint64>> result;
    result.reserve(static_cast<qsizetype>(affected.size()));
    for (const auto &[floor, key] : affected)
        result.append(qMakePair(floor, key));
    indexBlackTekZoneGeometry();
    return result;
}

void MapView::indexBlackTekZoneGeometry()
{
    m_blackTekOverlayZones = m_otbm && m_otbm->blackTekSpawnMode()
        ? m_otbm->blackTekZones() : QVector<BlackTekZone>();
    m_blackTekZoneTilesByChunk.clear();
    for (int zoneIndex = 0; zoneIndex < m_blackTekOverlayZones.size(); ++zoneIndex) {
        const BlackTekZone &zone = m_blackTekOverlayZones.at(zoneIndex);
        QHash<int, QSet<quint64>> zoneTiles;
        for (const auto &position : zone.worldPositions)
            zoneTiles[position.z].insert(posKey(position.x, position.y));

        constexpr int dx[] {-1, 1, 0, 0};
        constexpr int dy[] {0, 0, -1, 1};
        for (const auto &position : zone.worldPositions) {
            const auto &floorTiles = zoneTiles[position.z];
            quint8 edges = 0;
            for (int side = 0; side < 4; ++side) {
                if (!floorTiles.contains(posKey(position.x + dx[side], position.y + dy[side])))
                    edges |= quint8(1u << side);
            }
            const quint64 key = chunkKey(floorDiv(position.x, kChunkTiles),
                                         floorDiv(position.y, kChunkTiles));
            m_blackTekZoneTilesByChunk[position.z][key].append(
                {zoneIndex, position.x, position.y, false, false, edges});
        }
        const auto origin = zone.hasRange ? zone.rangeStart
            : BlackTekZone::Position3D{zone.minX, zone.minY, zone.floor};
        for (const auto &creature : zone.creatures) {
            auto position = creature.hasPosition ? creature.position
                : BlackTekZone::Position3D{};
            if (zone.playerRelative || !creature.hasPosition) {
                position.x += origin.x;
                position.y += origin.y;
                position.z += origin.z;
            }
            const quint64 key = chunkKey(floorDiv(position.x, kChunkTiles),
                                         floorDiv(position.y, kChunkTiles));
            m_blackTekZoneTilesByChunk[position.z][key].append(
                {zoneIndex, position.x, position.y, true, zone.playerRelative});
        }
    }
}

const CreatureStore::CreatureType *MapView::blackTekCreatureAt(int x, int y, int z) const
{
    if (!m_showCreatures)
        return nullptr;
    const auto floor = m_blackTekSpawnChunks.constFind(z);
    if (floor == m_blackTekSpawnChunks.cend())
        return nullptr;
    const quint64 key = chunkKey(floorDiv(x, kChunkTiles), floorDiv(y, kChunkTiles));
    const auto chunk = floor->constFind(key);
    if (chunk == floor->cend())
        return nullptr;
    const auto spawn = std::lower_bound(
        chunk->cbegin(), chunk->cend(), std::pair<int, int>{y, x},
        [](const BlackTekRenderSpawn &candidate, const std::pair<int, int> &position) {
            return candidate.y != position.first ? candidate.y < position.first
                                                 : candidate.x < position.second;
        });
    if (spawn == chunk->cend() || spawn->x != x || spawn->y != y || !spawn->hasCreature)
        return nullptr;
    return &spawn->creature;
}

void MapView::appendCreatureQuads(const CreatureStore::CreatureType *creature,
                                  int x, int y, int direction, int elevation,
                                  std::vector<QuadRef> &out) const
{
    if (!creature || !m_dat)
        return;

    const bool isOutfit = creature->lookType > 0;
    const ClientItem *definition = isOutfit
        ? m_dat->outfitByLookType(static_cast<uint16_t>(creature->lookType))
        : (creature->lookItem > 0 && m_otb
            ? m_dat->itemByClientId(static_cast<uint16_t>(
                m_otb->clientIdForServerId(creature->lookItem))) : nullptr);
    if (!definition || definition->sprite_ids.empty())
        return;

    const int width = std::max<int>(1, definition->width);
    const int height = std::max<int>(1, definition->height);
    const int layers = std::max<int>(1, definition->layers);
    const int patternX = std::max<int>(1, definition->pattern_x);
    const int outfitDirection = isOutfit ? ((direction % patternX) + patternX) % patternX : 0;
    const int renderedLayers = isOutfit ? 1 : layers;
    for (int layer = 0; layer < renderedLayers; ++layer) {
        for (int row = 0; row < height; ++row) {
            for (int column = 0; column < width; ++column) {
                const int index = ((outfitDirection * layers + layer) * height + row)
                                * width + column;
                if (index < 0 || index >= static_cast<int>(definition->sprite_ids.size()))
                    continue;
                const uint32_t spriteId = definition->sprite_ids[static_cast<size_t>(index)];
                const int atlasSlot = atlasSlotForSprite(spriteId);
                if (spriteId == 0 || atlasSlot < 0)
                    continue;
                out.push_back(QuadRef{
                    (x - column) * kSprite - elevation,
                    (y - row) * kSprite - elevation,
                    atlasSlot, false, x, y, true, 0, -2 });
            }
        }
    }
}

void MapView::updateCurrentFloor()
{

    m_minTileX = m_minTileY = m_maxTileX = m_maxTileY = 0;
    if (!m_otbm || !m_otbm->isLoaded()) return;

    const int floor = m_navigationController.floor();
    auto &bounds = m_chunkStore.bounds()[floor];
    auto &tileIndex = m_chunkStore.tiles();
    auto zit = tileIndex.find(floor);
    if (m_chunkStore.dirtyBounds().remove(floor)) {
        bounds = {};
        if (zit != tileIndex.end()) {
            for (auto cit = zit->begin(); cit != zit->end(); ++cit) {
                for (const OtbmTile *tile : cit.value()) {
                    bounds.include(tile->x, tile->y);
                }
            }
        }
        m_mapTileBounds[floor] = bounds;
        const auto spawnFloor = m_blackTekSpawnChunks.constFind(floor);
        if (spawnFloor != m_blackTekSpawnChunks.cend()) {
            for (auto chunk = spawnFloor->cbegin(); chunk != spawnFloor->cend(); ++chunk)
                for (const auto &spawn : chunk.value())
                    bounds.include(spawn.x, spawn.y);
        }
    }
    m_minTileX = bounds.minX; m_minTileY = bounds.minY;
    m_maxTileX = bounds.maxX; m_maxTileY = bounds.maxY;
}

void MapView::rebuildFloorIndex()
{
    {
        std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
        buildStaticIndex();
    }
    updateCurrentFloor();
    clearChunkQuadCache();
    ++m_dataVersion;
    m_minimapService.invalidate();
}

void MapView::appendItemQuads(const OtbmTile *tile, std::vector<QuadRef> &out,
                              bool *animated,
                              const CreatureStore::CreatureType *blackTekCreature) const
{
    // Metadata overlays may be edited before the client assets are available.
    if (!tile || !m_otb || !m_dat) return;

    int topIdx = -1;
    for (int i = static_cast<int>(tile->items.size()) - 1; i >= 0; --i) {
        const int cid = m_otb->clientIdForServerId(tile->items[static_cast<size_t>(i)].server_id);
        if (cid <= 0) continue;
        const ClientItem *c = m_dat->itemByClientId(static_cast<uint16_t>(cid));
        if (c && !c->sprite_ids.empty()) { topIdx = i; break; }
    }

    int elevation = 0;
    int hookPattern = -1;

    for (int idx = 0; idx < static_cast<int>(tile->items.size()); ++idx) {
        const OtbmMapItem &item = tile->items[static_cast<size_t>(idx)];
        const int clientId = m_otb->clientIdForServerId(item.server_id);
        if (clientId <= 0) continue;
        const ClientItem *ci = m_dat->itemByClientId(static_cast<uint16_t>(clientId));
        if (!ci || ci->sprite_ids.empty()) continue;

        const int w = std::max<int>(1, ci->width);
        const int h = std::max<int>(1, ci->height);
        const int layers = std::max<int>(1, ci->layers);
        const bool isGround = item.is_ground
                           || m_otb->isClientGroundForServerId(item.server_id);

        const bool isTop = (idx == topIdx) && tile->creature_name.isEmpty()
                           && !blackTekCreature;

        const int ox = ci->has(ClientProperty::Offset) ? ci->offset_x : 0;
        const int oy = ci->has(ClientProperty::Offset) ? ci->offset_y : 0;

        if (animated && ci->frames > 1) *animated = true;
        const int fr = itemFrame(ci);
        if (ci->has(ClientProperty::Hangable) && hookPattern < 0)
            hookPattern = hookPatternForTile(tile, m_otb, m_dat);

        for (int l = 0; l < layers; ++l)
            for (int hh = 0; hh < h; ++hh)
                for (int ww = 0; ww < w; ++ww) {
                    const uint32_t sid = cellSpriteId(ci, ww, hh, l, w, h, tile->x, tile->y,
                                                      tile->z, item.count, fr, hookPattern);
                    if (sid == 0) continue;
                    const int as = atlasSlotForSprite(sid);
                    if (as < 0) continue;

                    out.push_back(QuadRef{
                        (tile->x - ww) * kSprite - ox - elevation,
                        (tile->y - hh) * kSprite - oy - elevation,
                        as, isGround, tile->x, tile->y, isTop,

                        idx == 0 ? ((m_showZones && !m_modernZones ? static_cast<int>(tile->flags) : 0)
                                   | ((m_showHouses && !m_modernZones && tile->is_house) ? 64 : 0))
                                 : 0, idx });
                }

        if (ci->has(ClientProperty::Elevation)) elevation += ci->elevation;
    }

    if (m_showCreatures && !tile->creature_name.isEmpty() && m_creatureStore)
        appendCreatureQuads(m_creatureStore->byName(tile->creature_name),
                            tile->x, tile->y, tile->creature_direction,
                            elevation, out);
    else if (m_showCreatures && blackTekCreature)
        appendCreatureQuads(blackTekCreature, tile->x, tile->y, 0, elevation, out);
}

void MapView::appendTopItemQuads(const OtbmTile *tile, std::vector<QuadRef> &out) const
{

    std::vector<QuadRef> topQuads;
    int elevation = 0;
    int hookPattern = -1;

    for (const OtbmMapItem &item : tile->items) {
        const int clientId = m_otb->clientIdForServerId(item.server_id);
        if (clientId <= 0) continue;
        const ClientItem *ci = m_dat->itemByClientId(static_cast<uint16_t>(clientId));
        if (!ci || ci->sprite_ids.empty()) continue;

        const int w = std::max<int>(1, ci->width);
        const int h = std::max<int>(1, ci->height);
        const int layers = std::max<int>(1, ci->layers);
        const int ox = ci->has(ClientProperty::Offset) ? ci->offset_x : 0;
        const int oy = ci->has(ClientProperty::Offset) ? ci->offset_y : 0;
        const int elev = elevation;
        if (ci->has(ClientProperty::Elevation)) elevation += ci->elevation;

        const int fr = itemFrame(ci);
        if (ci->has(ClientProperty::Hangable) && hookPattern < 0)
            hookPattern = hookPatternForTile(tile, m_otb, m_dat);

        topQuads.clear();
        for (int l = 0; l < layers; ++l)
            for (int hh = 0; hh < h; ++hh)
                for (int ww = 0; ww < w; ++ww) {
                    const uint32_t sid = cellSpriteId(ci, ww, hh, l, w, h, tile->x, tile->y,
                                                      tile->z, item.count, fr, hookPattern);
                    if (sid == 0) continue;
                    const int as = atlasSlotForSprite(sid);
                    if (as < 0) continue;
                    topQuads.push_back(QuadRef{
                        (tile->x - ww) * kSprite - ox - elev,
                        (tile->y - hh) * kSprite - oy - elev,
                        as, item.is_ground
                            || m_otb->isClientGroundForServerId(item.server_id) });
                }
    }

    for (const QuadRef &q : topQuads) out.push_back(q);
}

void MapView::collectFloorChunkQuads(int z, quint64 key, std::vector<QuadRef> &out,
                                     bool *animated)
{
    const auto &tileIndex = m_chunkStore.tiles();
    const auto zit = tileIndex.constFind(z);
    const auto floorSpawns = m_blackTekSpawnChunks.constFind(z);
    const auto cit = zit != tileIndex.cend() ? zit->constFind(key)
        : QHash<quint64, std::vector<const OtbmTile *>>::const_iterator();
    const auto spawnIt = floorSpawns != m_blackTekSpawnChunks.cend()
        ? floorSpawns->constFind(key)
        : QHash<quint64, QVector<BlackTekRenderSpawn>>::const_iterator();
    const std::vector<const OtbmTile *> *tiles =
        zit != tileIndex.cend() && cit != zit->cend() ? &cit.value() : nullptr;
    const QVector<BlackTekRenderSpawn> *spawns =
        floorSpawns != m_blackTekSpawnChunks.cend() && spawnIt != floorSpawns->cend()
            ? &spawnIt.value() : nullptr;
    if (!m_showCreatures || !spawns || spawns->isEmpty()) {
        if (tiles)
            for (const OtbmTile *tile : *tiles)
                appendItemQuads(tile, out, animated);
        return;
    }

    const auto appendSpawn = [this, &out](const BlackTekRenderSpawn &spawn) {
        appendCreatureQuads(spawn.hasCreature ? &spawn.creature : nullptr,
                            spawn.x, spawn.y, 0, 0, out);
    };

    size_t tilePosition = 0;
    qsizetype spawnPosition = 0;
    while ((tiles && tilePosition < tiles->size()) || spawnPosition < spawns->size()) {
        const OtbmTile *tile = tiles && tilePosition < tiles->size()
            ? tiles->at(tilePosition) : nullptr;
        const auto *spawn = spawnPosition < spawns->size()
            ? &spawns->at(spawnPosition) : nullptr;
        const bool spawnFirst = spawn && (!tile || spawn->y < tile->y
            || (spawn->y == tile->y && spawn->x < tile->x));
        if (spawnFirst) {
            appendSpawn(*spawn);
            ++spawnPosition;
        } else if (tile) {
            const auto *creature = m_showCreatures && spawn
                && spawn->x == tile->x && spawn->y == tile->y
                && tile->creature_name.isEmpty() && spawn->hasCreature
                ? &spawn->creature : nullptr;
            appendItemQuads(tile, out, animated, creature);
            ++tilePosition;
            if (spawn && spawn->x == tile->x && spawn->y == tile->y) {
                ++spawnPosition;
            }
        }
    }
}

void MapView::startWorker()
{
    m_chunkStore.startWorker(
        [this](int floor, quint64 key, quint64 generation) {
            std::vector<QuadRef> quads;
            bool animated = false;
            std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
            if (!m_chunkStore.isCurrentRequest(generation)) return false;
            collectFloorChunkQuads(floor, key, quads, &animated);
            if (!m_chunkStore.isCurrentRequest(generation)) return false;
            storeChunkQuads(floor, key, std::move(quads), animated);
            if (initialViewToken()) LoadProfile::record(QStringLiteral("initial_chunk_ready"));
            return true;
        },
        [this] {
            QMetaObject::invokeMethod(this, [this] {
                emit contentUpdated();
                update();
            }, Qt::QueuedConnection);
        });
}

void MapView::stopWorker()
{
    m_chunkStore.stopWorker();
}

void MapView::requestChunkQuads(int z, quint64 key)
{
    m_chunkStore.requestChunk(z, key);
}

void MapView::storeChunkQuads(int z, quint64 key, std::vector<QuadRef> &&q, bool animated)
{
    std::lock_guard<std::recursive_mutex> dataLock(m_dataMutex);
    QSet<quint16> animationItems;
    if (animated && m_dat && m_otb) {
        const auto floor = m_chunkStore.tiles().constFind(z);
        if (floor != m_chunkStore.tiles().cend()) {
            for (const auto *tile : floor->value(key)) for (const auto &item : tile->items) {
                const int clientId = m_otb->clientIdForServerId(item.server_id);
                const auto *definition = m_dat->itemByClientId(static_cast<quint16>(clientId));
                if (definition && definition->frames > 1) {
                    animationItems.insert(static_cast<quint16>(clientId));
                    if (!m_animationPhases.contains(clientId))
                        m_animationPhases[clientId] = definition->animationFrameAt(quint64(m_animFrame) * 16);
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_chunkStore.cacheMutex());

        m_chunkStore.storeChunkLocked(
            z, key, std::make_shared<const std::vector<QuadRef>>(std::move(q)));

        quint32 &versionCounter = m_chunkStore.versionCounter();
        if (++versionCounter == 0 || versionCounter == kChunkPending)
            versionCounter = 1;
        m_chunkStore.versions()[z][key] = versionCounter;

        if (!animationItems.isEmpty()) {
            m_chunkStore.animatedChunks()[z].insert(key);
            m_chunkStore.animationItems()[z][key] = std::move(animationItems);
        }
        m_chunkStore.dirtyChunks().insert({z, key});
    }
    m_chunkStore.cacheVersion().fetch_add(1, std::memory_order_relaxed);
}

void MapView::renderTakeDirtyChunks(QVector<QPair<int, quint64>> &out)
{
    std::lock_guard<std::mutex> lk(m_chunkStore.cacheMutex());
    out.clear();
    auto &dirtyChunks = m_chunkStore.dirtyChunks();
    out.reserve(static_cast<qsizetype>(dirtyChunks.size()));
    for (const auto &[z, key] : dirtyChunks)
        out.append(qMakePair(z, key));
    dirtyChunks.clear();
    if (m_luaPreviewAnimationFrame!=m_animFrame) {
        for (const auto &chunk : m_luaPreviewAnimated)
            if (!out.contains(chunk)) out.append(chunk);
        m_luaPreviewAnimationFrame=m_animFrame;
    }
}

void MapView::refreshSelectionTint()
{

    QSet<quint64> nowSet;
    for (quint64 pk : m_selectionController.selected()) {
        const int x = selX(pk);
        const int y = selY(pk);
        nowSet.insert(chunkKey(floorDiv(x, kChunkTiles), floorDiv(y, kChunkTiles)));
    }

    QSet<quint64> dirty = nowSet;
    dirty.unite(m_selectionController.selectedChunks());
    if (dirty.isEmpty()) return;

    {
        std::lock_guard<std::mutex> lk(m_chunkStore.cacheMutex());

        auto &versions = m_chunkStore.versions();
        quint32 &versionCounter = m_chunkStore.versionCounter();
        for (auto vit = versions.begin(); vit != versions.end(); ++vit) {
            for (quint64 ck : dirty) {
                auto it = vit->find(ck);
                if (it == vit->end()) continue;

                if (++versionCounter == 0 || versionCounter == kChunkPending)
                    versionCounter = 1;
                it.value() = versionCounter;
                m_chunkStore.dirtyChunks().insert({vit.key(), ck});
            }
        }
    }
    m_selectionController.selectedChunks() = nowSet;
    m_chunkStore.cacheVersion().fetch_add(1, std::memory_order_relaxed);
}

void MapView::invalidateChunkQuads(int z, quint64 key)
{
    std::lock_guard<std::mutex> lk(m_chunkStore.cacheMutex());
    auto &versions = m_chunkStore.versions();
    m_chunkStore.removeChunkLocked(z, key);
    auto vit = versions.find(z);
    if (vit != versions.end()) vit->remove(key);
    m_chunkStore.dirtyChunks().insert({z, key});
}

void MapView::clearChunkQuadCache()
{

    std::lock_guard<std::recursive_mutex> dlk(m_dataMutex);
    m_chunkStore.invalidateRequests();
    {
        std::lock_guard<std::mutex> lk(m_chunkStore.cacheMutex());
        m_chunkStore.clearChunksLocked();
        m_chunkStore.versions().clear();
        m_chunkStore.animatedChunks().clear();
        m_chunkStore.dirtyChunks().clear();
    }
    // Ensure synchronize() observes the invalidated chunk cache.
    m_chunkStore.resetVersion().fetch_add(1, std::memory_order_relaxed);
    m_chunkStore.cacheVersion().fetch_add(1, std::memory_order_relaxed);
}
