pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import TinaIde

Item {
    id: view
    required property TerminalSession terminal
    property bool active: false
    property real bottomOverlayHeight: 0
    visible: active

    Flickable {
        id: outputFlick
        objectName: "terminalOutputViewport:" + view.terminal.sessionId
        anchors.fill: parent
        anchors.margins: 12
        clip: true
        contentWidth: width
        contentHeight: Math.max(height, outputText.height)
        boundsBehavior: Flickable.StopAtBounds
        property bool followOutput: true
        function scrollToEnd() {
            if (followOutput) contentY = Math.max(0, contentHeight - height)
        }
        onMovementEnded: followOutput = atYEnd
        onContentYChanged: {
            if (moving || verticalBar.pressed) followOutput = atYEnd
        }
        onHeightChanged: Qt.callLater(scrollToEnd)
        onContentHeightChanged: Qt.callLater(scrollToEnd)
        ScrollBar.vertical: ScrollBar { id: verticalBar }
        TextEdit {
            id: outputText
            objectName: "terminalOutput:" + view.terminal.sessionId
            width: outputFlick.width
            height: implicitHeight
            bottomPadding: view.bottomOverlayHeight
            text: view.active ? view.terminal.richOutput : ""
            textFormat: TextEdit.RichText
            readOnly: true
            selectByMouse: true
            selectByKeyboard: true
            persistentSelection: true
            wrapMode: TextEdit.WrapAnywhere
            color: "#dce8fa"
            selectionColor: "#8058a6ff"
            font.family: "monospace"
            font.pixelSize: 16
        }
    }
}
