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
    property real zoomFactor: 1
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
    readonly property bool readingMarkdown: document.markdown && document.markdownPreview
    function focusEditor() {
        if (!readingMarkdown) editor.forceActiveFocus()
    }

    RowLayout {
        id: markdownToolbar
        anchors.top: parent.top
        anchors.right: parent.right
        visible: pane.document.markdown
        height: visible ? 38 : 0
        Button {
            id: readButton
            objectName: "markdownReadButton:" + pane.document.path
            text: "阅读"
            flat: true
            highlighted: pane.readingMarkdown
            palette.buttonText: "#c7d9f1"
            palette.windowText: "#c7d9f1"
            palette.highlightedText: "#ffffff"
            background: Rectangle {
                radius: 6
                color: readButton.highlighted ? "#70304b70" : "#28101726"
            }
            onClicked: pane.document.markdownPreview = true
        }
        Button {
            id: sourceButton
            objectName: "markdownSourceButton:" + pane.document.path
            text: "源码"
            flat: true
            highlighted: !pane.readingMarkdown
            palette.buttonText: "#c7d9f1"
            palette.windowText: "#c7d9f1"
            palette.highlightedText: "#ffffff"
            background: Rectangle {
                radius: 6
                color: sourceButton.highlighted ? "#70304b70" : "#28101726"
            }
            onClicked: {
                pane.document.markdownPreview = false
                pane.focusEditor()
            }
        }
    }

    Item {
        id: previewHeader
        anchors.top: markdownToolbar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: pane.document.preview ? 32 : 0
        visible: pane.document.preview
        Label {
            anchors.fill: parent
            leftPadding: 8
            verticalAlignment: Text.AlignVCenter
            text: pane.document.name + (pane.document.fullDiff ? " · 完整文件 · 只读" : " · 只读")
                  + (pane.document.refreshing ? " · 获取中…" : "")
            color: "#b6c8e4"
            elide: Text.ElideMiddle
        }
    }

    ColumnLayout {
        id: conflictBar
        anchors.top: previewHeader.bottom
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
        visible: !pane.readingMarkdown
        anchors.top: conflictBar.bottom
        anchors.bottom: parent.bottom
        width: pane.document.fullDiff ? Math.max(96, 2 * metrics.advanceWidth(String(editor.lineCount)) + 36)
                                     : Math.max(48, metrics.advanceWidth(String(editor.lineCount)) + 20)
        clip: true
        readonly property var lines: {
            // 显式依赖排版版本；字号、文本或文档高度变化时重新取可见行的真实基线。
            const revision = pane.document.layoutRevision
            return pane.document.visibleLines(flick.contentY - editor.topPadding, height)
        }
        // 只创建屏幕附近的行号，长文件不生成成千上万个 QML 对象。
        Repeater {
            model: gutter.lines
            delegate: Text {
                required property var modelData
                objectName: "lineNumber:" + pane.document.path + ":" + modelData.number
                x: 0
                y: editor.topPadding + modelData.baseline - baselineOffset - flick.contentY
                width: gutter.width - 10
                text: pane.document.fullDiff
                      ? (modelData.oldNumber || "") + "  " + (modelData.newNumber || "") : modelData.number
                font: editor.font
                color: modelData.change > 0 ? "#8cdaa4" : modelData.change < 0 ? "#f19494" : "#6983a5"
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    // 背景固定覆盖可见整行，横向滚动和空白行也保持完整的红 / 绿底色。
    Item {
        anchors.top: conflictBar.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        visible: !pane.readingMarkdown && pane.document.preview
        clip: true
        z: -1
        Repeater {
            model: gutter.lines
            delegate: Rectangle {
                required property var modelData
                objectName: "diffLine:" + pane.document.path + ":" + modelData.number
                y: editor.topPadding + modelData.top - flick.contentY
                width: parent.width
                height: modelData.height
                color: modelData.change > 0 ? "#493ba55b" : modelData.change < 0 ? "#55c44848" : "transparent"
            }
        }
    }

    Flickable {
        id: flick
        visible: !pane.readingMarkdown
        objectName: "codeViewport:" + pane.document.path
        anchors.top: conflictBar.bottom
        anchors.bottom: parent.bottom
        anchors.left: gutter.right
        anchors.right: parent.right
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: !codeZoom.active
        contentWidth: Math.max(width, editor.contentWidth + 24)
        contentHeight: Math.max(height, editor.height)
        ScrollBar.vertical: ScrollBar {}
        ScrollBar.horizontal: ScrollBar {}

        function ensureCursorVisible() {
            if (!pane.active || !editor.activeFocus || codeZoom.active || codeZoom.adjusting)
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
            font.pixelSize: Math.round(16 * pane.zoomFactor)
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

    TextZoom {
        id: codeZoom
        objectName: "codeZoom:" + pane.document.path
        viewport: flick
        textItem: editor
        enabled: pane.active && !pane.readingMarkdown
        zoomFactor: pane.zoomFactor
        onZoomRequested: function(factor) { pane.zoomFactor = factor }
    }

    Loader {
        anchors.top: conflictBar.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        active: pane.readingMarkdown
        visible: active
        sourceComponent: MarkdownPane {
            document: pane.document
            bottomOverlayHeight: pane.bottomOverlayHeight
            zoomFactor: pane.zoomFactor
            onZoomRequested: function(factor) { pane.zoomFactor = factor }
        }
    }

    // 只在有状态时显示提示，不再为文件名和常驻标题栏占用代码高度。
    Label {
        anchors.top: conflictBar.bottom
        anchors.left: parent.left
        anchors.margins: 8
        text: pane.document.saving ? "保存中…" : pane.document.refreshing ? "获取中…"
              : pane.document.readOnly ? "只读" : ""
        visible: !pane.document.preview && text.length > 0
        color: "#b6c8e4"
        padding: 4
        background: Rectangle { color: "#d0101726"; radius: 4 }
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
