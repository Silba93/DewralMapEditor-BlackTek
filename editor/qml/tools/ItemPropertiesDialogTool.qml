import QtQuick
import Tibia 1.0
import "../dialogs"

ItemPropertiesDialog {
    required property var environment

    ctx: environment.propsDialog.contextOverride || environment.workspace.context
    mapCtrl: environment.workspace.mapView
    containerDialog: environment.containerDialog
    contextPath: environment.propsDialog.contextPathOverride
    onClosed: Qt.callLater(() => {
        environment.propsDialogLoader.active = false;
        environment.propsDialog.contextOverride = null;
        environment.propsDialog.contextPathOverride = [];
    })
}
