pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import TinaIde

Item {
    id: pane
    required property WorkspaceDocument document
    required property FfiClientBridge bridge
    property bool active: false
    property real bottomOverlayHeight: 0
    visible: active

    // 只动画透明度和 8 像素位移，不截图、不对编辑器做模糊或缩放。
    transform: Translate { id: slide }
    ParallelAnimation {
        id: enterAnimation
        NumberAnimation { target: pane; property: "opacity"; from: 0.55; to: 1; duration: 150 }
        NumberAnimation { target: slide; property: "y"; from: 8; to: 0; duration: 150; easing.type: Easing.OutCubic }
    }
    onActiveChanged: {
        if (active) {
            enterAnimation.restart()
            Qt.callLater(focusEditor)
        }
    }
    function focusEditor() { editor.forceActiveFocus() }

    RowLayout {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 44
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: pane.document.path + (pane.document.dirty ? "  • 未保存" : "")
            color: pane.document.dirty ? "#ffd18b" : "#99b4d7"
            elide: Text.ElideMiddle
            font.pixelSize: 14
        }
        Label {
            text: pane.document.saving ? "保存中…" : pane.document.refreshing ? "获取中…"
                  : pane.document.readOnly ? "只读" : ""
            color: "#99b4d7"
        }
    }

    ColumnLayout {
        id: conflictBar
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        visible: pane.document.conflict
        height: visible ? implicitHeight : 0
        Label {
            text: "服务端版本已变化，本地修改仍保留。比较后选择要保存的版本。"
            color: "#ffd18b"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Button { text: "查看服务端版本"; onClicked: remoteDialog.open() }
            Button {
                text: "保留本地并保存"
                enabled: !pane.document.stale && !pane.document.refreshing && !pane.document.saving
                onClicked: pane.bridge.saveLocalVersion()
            }
            Button {
                text: "采用服务端版本"
                enabled: !pane.document.stale && !pane.document.refreshing && !pane.document.saving
                onClicked: pane.bridge.useServerVersion()
            }
        }
    }

    FontMetrics { id: metrics; font: editor.font }

    Item {
        id: gutter
        anchors.top: conflictBar.bottom
        anchors.bottom: parent.bottom
        width: 48
        clip: true
        readonly property int firstLine: Math.max(0, Math.floor(flick.contentY / metrics.height))
        // 只创建屏幕附近的行号，长文件不生成成千上万个 QML 对象。
        Repeater {
            model: Math.max(0, Math.min(editor.lineCount - gutter.firstLine,
                                        Math.ceil(gutter.height / metrics.height) + 2))
            delegate: Text {
                required property int index
                x: 0
                y: 8 + (gutter.firstLine + index) * metrics.height - flick.contentY
                width: gutter.width - 10
                height: metrics.height
                text: gutter.firstLine + index + 1
                font: editor.font
                color: "#6983a5"
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    Flickable {
        id: flick
        anchors.top: conflictBar.bottom
        anchors.bottom: parent.bottom
        anchors.left: gutter.right
        anchors.right: parent.right
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: Math.max(width, editor.contentWidth + 24)
        contentHeight: Math.max(height, editor.height)
        ScrollBar.vertical: ScrollBar {}
        ScrollBar.horizontal: ScrollBar {}

        function ensureCursorVisible() {
            if (!pane.active || !editor.activeFocus)
                return
            const rect = editor.cursorRectangle
            if (rect.x < contentX) contentX = rect.x
            if (rect.x + rect.width > contentX + width)
                contentX = rect.x + rect.width - width + 16
            // 编辑时让光标避开悬浮输入框，滚动时仍可看到输入框后面的代码。
            const unobscuredHeight = Math.max(1, height - pane.bottomOverlayHeight)
            if (rect.y < contentY) contentY = rect.y
            if (rect.y + rect.height > contentY + unobscuredHeight)
                contentY = rect.y + rect.height - unobscuredHeight
        }

        TextEdit {
            id: editor
            objectName: "editor:" + pane.document.path
            width: Math.max(flick.width, contentWidth + 24)
            height: Math.max(flick.height, contentHeight + topPadding + bottomPadding)
            topPadding: 8
            bottomPadding: pane.bottomOverlayHeight + 8
            text: pane.document.text
            textFormat: TextEdit.PlainText
            readOnly: pane.document.readOnly
            color: "#e2ecff"
            selectionColor: "#8058a6ff"
            selectedTextColor: "#ffffff"
            font.family: "monospace"
            font.pixelSize: 16
            wrapMode: TextEdit.NoWrap
            selectByMouse: true
            selectByKeyboard: true
            persistentSelection: true
            tabStopDistance: metrics.advanceWidth("    ")
            // 禁用代码输入的自动首字母大写和预测替换，兼容 Android 输入法及外接键盘。
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhMultiLine
            onTextChanged: pane.document.text = text
            onCursorRectangleChanged: flick.ensureCursorVisible()
            Component.onCompleted: {
                pane.document.attach(textDocument)
                if (pane.active) Qt.callLater(pane.focusEditor)
            }
            Keys.onTabPressed: function(event) {
                if (!readOnly) {
                    const start = selectionStart
                    remove(selectionStart, selectionEnd)
                    insert(start, "    ")
                    cursorPosition = start + 4
                    event.accepted = true
                }
            }
        }
    }

    Dialog {
        id: remoteDialog
        title: "服务端当前版本 · " + pane.document.name
        anchors.centerIn: parent
        width: Math.min(pane.width, 820)
        height: Math.min(pane.height, 580)
        modal: true
        standardButtons: Dialog.Close
        ScrollView {
            anchors.fill: parent
            TextArea {
                text: pane.document.remoteText
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                font.family: "monospace"
                wrapMode: TextEdit.NoWrap
            }
        }
    }
}
