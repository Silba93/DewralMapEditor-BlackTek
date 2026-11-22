import QtQuick
import "../themes/tibia" as TibiaSkin
import QtQuick.Controls
import QtQuick.Window
import Tibia 1.0
import "../style"
import "../themes/fluent/Colors.js" as Colors

Item {
    id: root

    required property var mapCtrl
    property bool compact: false
    property bool sectionFrame: false
    property bool collapsed: false
    readonly property string brushSummary: (mapCtrl.brushSize*2+1)+" × "+(mapCtrl.brushSize*2+1)+" · "+(mapCtrl.brushShape==="circle" ? "Circle" : "Square")
    readonly property bool framed: sectionFrame && Backend.uiTheme.style === "tibia-ui" && !compact
    Loader {
        objectName: "brushSectionFrame"
        active: root.framed
        anchors.fill: parent
        sourceComponent: TibiaSkin.TibiaPopupWindow {
            caption: "Brush size"; showCloseButton: false; showCollapseButton: true
            collapsed: root.collapsed
            onCollapseRequested: root.collapsed=!root.collapsed
        }
    }
    signal dockDragStarted()
    signal dockDragMoved(real sceneX, real sceneY)
    signal dockDragFinished(real sceneX, real sceneY)
    signal dockDragCanceled()
    MouseArea {
        id: dockHandle
        objectName: "brushDockHandle"
        z: 5
        x: 0; y: 0
        width: root.compact ? 18 : root.framed ? Math.max(0,root.width-24) : root.width
        height: root.compact ? root.height : root.framed ? 17 : 30
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        property bool moving: false
        property point origin
        function scene(mouse) { return mapToItem(root.Window.window.contentItem, mouse.x, mouse.y); }
        onPressed: mouse => { moving = false; origin = scene(mouse); }
        onPositionChanged: mouse => {
            if (!pressed) return;
            const p = scene(mouse);
            if (!moving && Math.abs(p.x - origin.x) + Math.abs(p.y - origin.y) > 6) {
                moving = true; root.dockDragStarted();
            }
            if (moving) root.dockDragMoved(p.x, p.y);
        }
        onReleased: mouse => { if (moving) { const p = scene(mouse); root.dockDragFinished(p.x, p.y); } moving = false; }
        onCanceled: { moving = false; root.dockDragCanceled(); }
        Text { visible: root.compact; anchors.centerIn: parent; text: "⋮"; color: Colors.c("muted"); font.pixelSize: 20 }
    }
    readonly property bool tibiaUi: Backend.uiTheme.style === "tibia-ui"
    readonly property bool doodadSelected: !!mapCtrl.doodadBrush && mapCtrl.doodadBrush.length > 0
    readonly property int brushButtonCount: compact && doodadSelected ? 10 : 9
    // Keep the brush controls in one row, including the separator.
    readonly property real brushButtonSize: root.compact
            ? Math.min(root.tibiaUi ? 26 : 24,
                       Math.max(1, Math.floor((brushContent.width - 6 - root.brushButtonCount * brushButtons.spacing) / root.brushButtonCount)))
            : root.tibiaUi ? Math.min(32, Math.max(1, Math.floor((brushContent.width - 6 - 9 * brushButtons.spacing) / 9))) : 24

    implicitHeight: compact ? 30 : framed ? (collapsed ? 17 : 25+brushContent.implicitHeight+8) : brushContent.implicitHeight + 16
    Rectangle {
        anchors.fill: parent
        visible: !root.compact && !root.framed
        radius: 6
        color: Colors.c("surface")
        border.width: 1
        border.color: Colors.c("border")
        TibiaSkin.TibiaSkin { anchors.fill: parent }
    }
    Column {
    id: brushContent
    visible: !root.framed || !root.collapsed
    x: root.compact ? 20 : root.framed ? 8 : 8
    y: root.compact ? 3 : root.framed ? 25 : 8
    width: root.width - (root.compact ? 20 : root.framed ? 16 : 16)
    spacing: root.compact ? 0 : 8


    Text {
        id: brushHeading
        visible: !root.compact && !root.framed
        text: "Brush size"
        color: (Colors.c("heading"))
        font.pixelSize: 11
        font.bold: false
        font.family: (Colors.family())
    }

    Flow {
        id: brushButtons
        width: parent.width
        spacing: 3

        Repeater {
            model: ["square", "circle"]
            delegate: PaletteBrushButton {
                required property string modelData
                objectName: "fluentBrush" + modelData
                width: root.brushButtonSize
                height: root.brushButtonSize
                active: root.mapCtrl.brushShape === modelData
                round: modelData === "circle"
                iconSize: 14
                Accessible.name: modelData === "circle" ? "Circle brush" : "Square brush"
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: modelData === "circle" ? "Circle brush" : "Square brush"
                onClicked: root.mapCtrl.brushShape = modelData
            }
        }

        Item {
            width: 6
            height: root.brushButtonSize
        }

        Repeater {
            model: [0, 1, 2, 4, 6, 8, 11]
            delegate: PaletteBrushButton {
                required property int modelData
                required property int index
                objectName: "brushRadius" + modelData
                width: root.brushButtonSize
                height: root.brushButtonSize
                active: root.mapCtrl.brushSize === modelData
                round: root.mapCtrl.brushShape === "circle"
                iconSize: 6 + index * 2
                onClicked: root.mapCtrl.brushSize = modelData
                ToolTip.visible: Backend.uiTheme.style !== "fluent-dark" && Backend.uiTheme.style !== "tibia-ui" && hovered
                ToolTip.text: (modelData * 2 + 1) + "x" + (modelData * 2 + 1)

                DmeToolTip {
                    targetItem: hoverArea
                    targetHovered: hovered
                    message: (modelData * 2 + 1) + "x" + (modelData * 2 + 1)
                }
            }
        }

        PaletteBrushButton {
            visible: root.compact && root.doodadSelected
            width: visible ? root.brushButtonSize : 0
            height: root.brushButtonSize
            iconSize: 0
            Accessible.name: "Doodad density"
            ToolTip.visible: hovered
            ToolTip.text: "Doodad density: " + root.mapCtrl.doodadDensity + "/10"
            onClicked: compactDensityPopup.open()
            Text {
                anchors.centerIn: parent
                text: "D"
                color: Colors.c("text")
                font.pixelSize: 10
            }
        }
    }

    Row {
        visible: !root.compact && root.doodadSelected
        width: parent.width
        height: visible ? 26 : 0
        spacing: 6

        Text {
            id: densityLabel
            text: "Density"
            color: Colors.c("heading")
            font.pixelSize: 11
            anchors.verticalCenter: parent.verticalCenter
        }

        Slider {
            id: doodadDensitySlider
            width: Math.max(45, parent.width - densityLabel.implicitWidth - densityValue.implicitWidth - parent.spacing * 2)
            from: 1
            to: 10
            stepSize: 1
            snapMode: Slider.SnapAlways
            value: root.mapCtrl.doodadDensity
            anchors.verticalCenter: parent.verticalCenter
            onMoved: root.mapCtrl.doodadDensity = Math.round(value)
            ToolTip.visible: hovered
            ToolTip.text: "Doodad density: " + Math.round(value) + "/10"
        }

        Text {
            id: densityValue
            text: Math.round(doodadDensitySlider.value) + "/10"
            color: Colors.c("muted")
            font.pixelSize: 10
            anchors.verticalCenter: parent.verticalCenter
        }
    }
    }

    Popup {
        id: compactDensityPopup
        parent: root.Window.window ? root.Window.window.contentItem : root
        readonly property point anchorPosition: root.mapToItem(parent, 0, root.height)
        x: anchorPosition.x
        y: anchorPosition.y
        width: 250
        padding: 12
        contentItem: Row {
            spacing: 8
            Text {
                text: "Density"
                color: Colors.c("heading")
                anchors.verticalCenter: parent.verticalCenter
            }
            Slider {
                width: 170
                from: 1
                to: 10
                stepSize: 1
                snapMode: Slider.SnapAlways
                value: root.mapCtrl.doodadDensity
                anchors.verticalCenter: parent.verticalCenter
                onMoved: root.mapCtrl.doodadDensity = Math.round(value)
                ToolTip.visible: hovered
                ToolTip.text: Math.round(value) + "/10"
            }
            Text {
                text: root.mapCtrl.doodadDensity + "/10"
                color: Colors.c("muted")
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }
}
