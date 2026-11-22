pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Tibia 1.0
import "../style"

DmeDialog {
    id: dialog
    title: "BlackTek zones"
    width: 760
    height: 680
    modal: false
    dim: false
    required property var reader
    required property var mapView
    property string selectedFile: ""
    property string selectedZone: ""
    property string status: ""
    property string rangeStart: ""
    property string sourceBaseline: ""
    property string sourceFolder: ""
    property int zoneRevision: 0
    readonly property var zoneGeometry: {
        const revision = zoneRevision
        return selectedZone && selectedFile
            ? reader.blackTekZoneGeometry(selectedFile, selectedZone) : ({})
    }
    readonly property var groupEntries: (zoneGeometry.creatures || []).filter(
        entry => entry.category === spawnGroup.currentText)
    readonly property var selectedSpawn: groupEntries[spawnEntry.currentIndex] || null
    function spawnFieldPath(entry, fieldName) {
        return entry.category === "master" ? "master." + fieldName
            : entry.category + "." + entry.index + "." + fieldName
    }
    function locateFolder() { picker.open() }
    function mapPosition() {
        return mapView.hoverX < 0 || mapView.hoverY < 0 ? ""
            : mapView.hoverX + ":" + mapView.hoverY + ":" + mapView.floor
    }
    function fieldChoices(key) {
        switch (key) {
        case "spawn_type": return ["monster", "npc", "boss", "item"]
        case "policy": return ["fixed", "relative", "staged", "triggered"]
        case "trigger": return ["Spawn", "Enter", "Leave", "Death", "Despawn", "Login", "Logout",
                "Use", "Remove", "Transform", "Speak", "Look", "Summon", "Equip", "DeEquip",
                "Attack", "Defend", "Idle", "GainSkull", "LoseSkull", "Kill", "EnterBattle", "LeaveBattle"]
        case "minion_behavior": return ["despawn_on_master_death", "finish_life_only", "independent_respawn"]
        case "flags": return ["protection", "nopvp", "pvp", "nologout", "no_experience",
                "no_trading", "no_transaction", "no_party", "no_drop", "no_death",
                "no_death_penalty", "no_equipment_loss", "no_summons", "ghost",
                "no_pathfinding", "no_walk", "no_monsters", "no_npcs"]
        case "weekdays": return ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]
        case "passive": case "forced": case "rebootable": case "resumable": case "degradable": case "timed":
            return ["true", "false"]
        default: return []
        }
    }
    signal revealZone(string path, string name)
    onReaderChanged: close()
    function openSource(path) {
        selectedFile = path
        sourceFolder = reader.blackTekZoneFolder
        sourceBaseline = reader.blackTekZoneFileText(path)
        source.text = sourceBaseline
    }
    Connections {
        target: dialog.reader
        function onBlackTekZonesChanged() {
            ++dialog.zoneRevision
            if (!dialog.selectedFile) return
            if (dialog.sourceFolder !== dialog.reader.blackTekZoneFolder) {
                dialog.selectedFile = ""
                dialog.selectedZone = ""
                source.text = ""
                dialog.status = "The zone folder changed. Select a zone to continue editing."
                return
            }
            if (source.text === dialog.sourceBaseline) dialog.openSource(dialog.selectedFile)
        }
    }
    function spawnOffset() {
        const zone = zoneGeometry
        if (zone.playerRelative) return relativeOffset.text.trim()
        const position = mapPosition()
        if (!position || zone.originX === undefined) return ""
        const offset = position.split(":").map(Number)
        offset[0] -= zone.originX
        offset[1] -= zone.originY
        offset[2] -= zone.originZ
        return offset.some(value => value < 0) ? "" : offset.join(":")
    }
    onAboutToShow: {
        selectedFile = ""
        selectedZone = ""
        source.text = ""
        status = ""
    }

    FolderDialog {
        id: picker
        title: "Locate BlackTek zones folder"
        onAccepted: {
            if (!dialog.reader.linkBlackTekZones(Backend.fileTools.toLocalFile(selectedFolder))) {
                dialog.status = "Cannot link this folder. Save pending changes before switching folders."
                return
            }
            dialog.selectedFile = ""
            source.text = ""
        }
    }

    ColumnLayout {
        anchors.fill: parent
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: dialog.reader.blackTekZoneFolder
            }
            DmeButton {
                text: "Open zones.toml"
                enabled: dialog.reader.blackTekZoneFolder.length > 0
                onClicked: {
                    dialog.selectedZone = ""
                    dialog.openSource(dialog.reader.blackTekZoneFolder + "/zones.toml")
                }
            }
            DmeButton { text: "Locate zones folder…"; onClicked: picker.open() }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: dialog.status
            visible: text.length > 0
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(96, diagnostics.implicitHeight)
            visible: diagnostics.text.length > 0
            TextArea {
                id: diagnostics
                readOnly: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                text: dialog.reader.blackTekZoneErrors.join("\n")
            }
        }
        DmeTextField { id: filter; Layout.fillWidth: true; placeholderText: "Search zones" }
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: 160
            clip: true
            model: dialog.reader.blackTekZoneEntries.filter(entry =>
                entry.name.toLowerCase().includes(filter.text.toLowerCase()))
            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                DmeButton {
                    Layout.fillWidth: true
                    text: parent.modelData.name
                    onClicked: {
                        const path = parent.modelData.sourceFile
                        dialog.selectedZone = parent.modelData.name
                        dialog.openSource(path)
                        dialog.status = ""
                    }
                }
                DmeButton {
                    text: "Go to zone"
                    onClicked: {
                        const entry = parent.modelData
                        dialog.mapView.centerOnTile(entry.x, entry.y, entry.z)
                        dialog.revealZone(entry.sourceFile, entry.name)
                        dialog.close()
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            text: dialog.selectedFile || "Select a zone to edit its source TOML file."
            elide: Text.ElideMiddle
        }
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.selectedZone.length > 0
            DmeTextField { id: renameField; Layout.fillWidth: true; placeholderText: "New name for selected zone" }
            DmeButton {
                text: "Rename zone"
                enabled: renameField.text.trim().length > 0
                onClicked: {
                    const name = renameField.text.trim()
                    const updated = dialog.reader.changeBlackTekZoneName(source.text, dialog.selectedZone, name)
                    if (!updated) { dialog.status = "Could not rename the selected zone."; return }
                    source.text = updated
                    dialog.selectedZone = name
                    dialog.status = "Review and apply the rename."
                }
            }
            DmeButton {
                text: "Remove zone"
                onClicked: {
                    const updated = dialog.reader.changeBlackTekZoneName(source.text, dialog.selectedZone, "")
                    if (!updated) { dialog.status = "Could not remove the selected zone."; return }
                    source.text = updated
                    dialog.selectedZone = ""
                    dialog.status = "Review and apply the removal."
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.selectedZone.length > 0
            ComboBox {
                id: field
                model: ["range", "positions", "flags", "spawn_type", "policy", "monsters",
                        "npcs", "master", "minions", "cooldown", "trigger", "minion_behavior",
                        "weekdays", "passive", "forced", "rebootable", "resumable", "degradable", "timed"]
            }
            ComboBox {
                id: fieldPreset
                visible: count > 0
                model: dialog.fieldChoices(field.currentText)
                onActivated: {
                    const key = field.currentText
                    if (key === "flags" || key === "weekdays") {
                        const updated = dialog.reader.replaceBlackTekZoneField(
                            source.text, dialog.selectedZone, key, JSON.stringify(currentText), true)
                        if (!updated) {
                            dialog.status = "Could not add to the list. Check its TOML value."
                            return
                        }
                        source.text = updated
                        dialog.status = "List entry added. Review and apply the change."
                    } else {
                        fieldValue.text = currentText === "true" || currentText === "false"
                            ? currentText : JSON.stringify(currentText)
                    }
                }
            }
            DmeTextField {
                id: fieldValue
                Layout.fillWidth: true
                placeholderText: "New TOML value"
            }
            DmeButton {
                text: "Set field"
                onClicked: {
                    const updated = dialog.reader.replaceBlackTekZoneField(
                        source.text, dialog.selectedZone, field.currentText, fieldValue.text, false)
                    if (!updated) {
                        dialog.status = "Could not set that field. Check the source TOML."
                        return
                    }
                    source.text = updated
                    dialog.status = "Review the value, then apply the change."
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            DmeTextField { id: newZoneName; Layout.fillWidth: true; placeholderText: "New zone name" }
            DmeButton {
                text: "Add zone at hovered tile"
                enabled: newZoneName.text.trim().length > 0 && dialog.reader.blackTekZoneFolder.length > 0
                onClicked: {
                    const position = dialog.mapPosition()
                    if (!position) { dialog.status = "Hover a map tile first."; return }
                    if (!dialog.selectedFile) {
                        dialog.openSource(dialog.reader.blackTekZoneFolder + "/zones.toml")
                    }
                    dialog.selectedZone = newZoneName.text.trim()
                    source.append("\n[" + JSON.stringify(dialog.selectedZone) + "]\npositions = ["
                                  + JSON.stringify(position) + "]")
                    dialog.status = "Review the new zone and apply the change."
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: "Hover a map tile, then capture its coordinates." }
            DmeButton {
                text: "Capture start"
                onClicked: {
                    dialog.rangeStart = dialog.mapPosition()
                    dialog.status = dialog.rangeStart ? "Start: " + dialog.rangeStart : "Hover a map tile first."
                }
            }
            DmeButton {
                text: "Capture end"
                enabled: dialog.rangeStart.length > 0
                onClicked: {
                    const end = dialog.mapPosition()
                    if (!end) return
                    field.currentIndex = 0
                    fieldValue.text = "{ start = " + JSON.stringify(dialog.rangeStart)
                                    + ", end = " + JSON.stringify(end) + " }"
                }
            }
            DmeButton {
                text: "Insert position"
                onClicked: {
                    const position = dialog.mapPosition()
                    if (position) source.insert(source.cursorPosition, JSON.stringify(position))
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.selectedZone.length > 0
            ComboBox {
                id: spawnGroup
                model: ["monsters", "npcs", "master", "minions"]
            }
            ComboBox {
                id: spawnEntry
                Layout.fillWidth: true
                model: dialog.groupEntries.map(entry => (entry.index + 1) + ". " + (entry.name || "Creature list")
                    + (entry.hasPosition ? " (" + entry.position.x + ":" + entry.position.y + ":" + entry.position.z + ")" : ""))
            }
            DmeTextField {
                id: relativeOffset
                visible: !!dialog.zoneGeometry.playerRelative
                placeholderText: "Player-relative offset"
            }
            DmeButton {
                text: dialog.zoneGeometry.playerRelative ? "Set spawn offset" : "Move spawn to hovered tile"
                enabled: spawnEntry.currentIndex >= 0
                onClicked: {
                    const offset = dialog.spawnOffset()
                    if (!offset) {
                        dialog.status = dialog.zoneGeometry.playerRelative
                            ? "Enter an offset such as 2:0:0." : "Hover the destination tile at or after the zone origin."
                        return
                    }
                    const path = dialog.spawnFieldPath(dialog.selectedSpawn, "position")
                    const updated = dialog.reader.replaceBlackTekZoneField(
                        source.text, dialog.selectedZone, path, JSON.stringify(offset), false)
                    if (!updated) { dialog.status = "Could not set the position in this spawn entry."; return }
                    source.text = updated
                    dialog.status = "Spawn position updated. Apply the change to preview it."
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.selectedZone.length > 0
            DmeTextField { id: creatureName; Layout.fillWidth: true; placeholderText: "Creature name" }
            DmeButton {
                text: dialog.zoneGeometry.playerRelative ? "Add spawn with offset" : "Add spawn at hovered tile"
                enabled: creatureName.text.trim().length > 0
                onClicked: {
                    const offset = dialog.spawnOffset()
                    if (!offset) {
                        dialog.status = dialog.zoneGeometry.playerRelative
                            ? "Enter an offset such as 2:0:0." : "Hover a tile at or after the zone origin."
                        return
                    }
                    const entry = "{ name = " + JSON.stringify(creatureName.text.trim())
                                + ", position = " + JSON.stringify(offset) + " }"
                    const group = spawnGroup.currentText
                    const updated = dialog.reader.replaceBlackTekZoneField(
                        source.text, dialog.selectedZone, group, entry, group !== "master")
                    if (!updated) {
                        dialog.status = "Could not add to that spawn group. Check the source TOML."
                        return
                    }
                    source.text = updated
                    dialog.status = "Spawn added. Apply the change to preview it."
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: dialog.selectedSpawn !== null
            ComboBox {
                id: spawnSetting
                model: spawnGroup.currentText === "npcs" ? ["interval"]
                    : spawnGroup.currentText === "minions" ? ["interval", "chance", "delay"]
                    : spawnGroup.currentText === "monsters" && dialog.zoneGeometry.staged
                        ? ["interval", "chance", "wave"] : ["interval", "chance"]
            }
            SpinBox {
                id: spawnSettingValue
                from: 0
                to: spawnSetting.currentText === "chance" ? 100
                    : spawnSetting.currentText === "wave" ? 255
                    : spawnSetting.currentText === "delay" ? 2147483647 : 4294967
                editable: true
                value: dialog.selectedSpawn ? dialog.selectedSpawn[spawnSetting.currentText] || 0 : 0
            }
            Label {
                text: spawnSetting.currentText === "chance" ? "%"
                    : spawnSetting.currentText === "wave" ? ""
                    : spawnSetting.currentText === "delay" ? "milliseconds" : "seconds"
            }
            DmeButton {
                text: "Set spawn rule"
                enabled: !!dialog.selectedSpawn
                onClicked: {
                    const path = dialog.spawnFieldPath(dialog.selectedSpawn, spawnSetting.currentText)
                    const updated = dialog.reader.replaceBlackTekZoneField(
                        source.text, dialog.selectedZone, path, String(spawnSettingValue.value), false)
                    if (!updated) { dialog.status = "Could not set that spawn rule."; return }
                    source.text = updated
                    dialog.status = "Review and apply the updated spawn rule."
                }
            }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            TextArea { id: source; enabled: dialog.selectedFile !== ""; wrapMode: TextEdit.NoWrap; textFormat: TextEdit.PlainText }
        }
        DmeButton {
            text: "Apply TOML changes"
            enabled: dialog.selectedFile !== ""
            onClicked: {
                if (dialog.reader.blackTekZoneFileText(dialog.selectedFile) !== dialog.sourceBaseline) {
                    dialog.status = "The file changed in this document. Reopen it before applying edits."
                    return
                }
                const error = dialog.reader.editBlackTekZoneFile(dialog.selectedFile, source.text)
                if (!error) dialog.openSource(dialog.selectedFile)
                dialog.status = error || "Changes applied to the document. Save the map to write them."
            }
        }
    }
}
