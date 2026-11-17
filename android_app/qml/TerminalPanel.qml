pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import TinaIde

Item {
    id: panel
    required property FfiClientBridge bridge
    required property Item blurSource
    property bool folderOpen: false
    property string commandError: ""
    property alias inputText: cmdInput.text
    readonly property TerminalSession terminal: bridge.activeTerminal
    signal folderRequested(bool projects)
    signal submitted()
    height: 43

    function focusInput() { cmdInput.forceActiveFocus() }
    onTerminalChanged: {
        cmdInput.text = terminal ? terminal.draft : ""
    }
    Connections {
        target: panel.terminal
        function onDraftChanged() { cmdInput.text = panel.terminal.draft }
    }

    GlassPanel {
        anchors.fill: parent
        blurSource: panel.blurSource
        cornerRadius: height / 2
        blurRadius: 32
        highlightGlow: cmdInput.activeFocus
        tintColor: cmdInput.activeFocus ? "#82141e32" : "#68101726"
        borderColor: cmdInput.activeFocus ? "#886ab4ff" : "#408aa8d8"
    }
    // 空白区域截住点击，避免误点到面板后面的代码。
    MouseArea { anchors.fill: parent }

    RowLayout {
        id: inputRow
        objectName: "terminalInputRow"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 43
        spacing: 10
        Rectangle {
            id: folderButton
            objectName: "folderButton"
            Layout.preferredWidth: 43
            Layout.fillHeight: true
            radius: height / 2
            color: panel.folderOpen ? "#483b82f6"
                   : folderMouse.containsMouse ? "#32ffffff" : "#1cffffff"
            border.color: panel.folderOpen ? "#7cc4ff" : "#388aa8d8"
            border.width: 1.1
            UiIcon {
                anchors.centerIn: parent
                name: "folder"
                color: panel.folderOpen ? "#ffffff" : "#d0e0ff"
            }
            MouseArea {
                id: folderMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: panel.folderRequested(false)
                onPressAndHold: panel.folderRequested(true)
            }
        }
        TextField {
            id: cmdInput
            objectName: "commandInput"
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#f0f6ff"
            selectionColor: "#4058a6ff"
            selectedTextColor: "#ffffff"
            font.family: "monospace"
            font.pixelSize: 16
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            background: Item {}
            onTextChanged: if (panel.terminal) panel.terminal.draft = text
            onAccepted: panel.submitted()
            ToolTip.visible: panel.commandError.length > 0
            ToolTip.text: panel.commandError
            ToolTip.timeout: 6000
            Keys.onTabPressed: function(event) {
                panel.bridge.completeTerminalInput()
                event.accepted = true
            }
            Keys.onPressed: function(event) {
                if (event.modifiers & Qt.ControlModifier) {
                    if (event.key === Qt.Key_C && selectedText.length === 0) {
                        panel.bridge.interruptTerminal()
                        event.accepted = true
                    } else if (event.key === Qt.Key_L && panel.terminal) {
                        panel.terminal.clear()
                        event.accepted = true
                    }
                }
            }
        }
        Rectangle {
            id: submitButton
            objectName: "submitButton"
            Layout.preferredWidth: 43
            Layout.fillHeight: true
            radius: height / 2
            color: submitMouse.pressed ? "#2563eb" : submitMouse.containsMouse ? "#3b82f6" : "#38284878"
            border.color: "#788cc0ff"
            border.width: 1.2
            UiIcon {
                anchors.centerIn: parent
                name: "arrow-up"
                color: "#f4f8ff"
                width: 18
                height: 18
            }
            MouseArea {
                id: submitMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: panel.submitted()
            }
        }
    }
}
