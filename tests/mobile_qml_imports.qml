import QtQuick
import CodeHarbor.Mobile
import CodeHarbor.Mobile.Core

Item {
    width: 640
    height: 480

    MobileTerminalView {
        id: terminal
        anchors.fill: parent
        font.pixelSize: 14
        property int reportedColumns: terminal.columns
        property int reportedRows: terminal.rows
    }

    MobileBusy {
        message: "Waiting for fixture metadata"
    }
}
