pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import TinaIde

Flickable {
    id: view
    required property WorkspaceDocument document
    property real bottomOverlayHeight: 0
    objectName: "markdownViewport:" + document.path
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    contentWidth: width
    contentHeight: Math.max(height, content.height)
    ScrollBar.vertical: ScrollBar {}
    TextEdit {
        id: content
        objectName: "markdownContent:" + view.document.path
        width: view.width
        height: implicitHeight
        padding: 20
        bottomPadding: view.bottomOverlayHeight + 24
        text: view.document.text
        textFormat: TextEdit.MarkdownText
        readOnly: true
        selectByMouse: true
        selectByKeyboard: true
        persistentSelection: true
        wrapMode: TextEdit.Wrap
        color: "#e2ecff"
        selectionColor: "#8058a6ff"
        font.pixelSize: 18
        onLinkActivated: function(link) {
            if (/^https?:\/\//i.test(link) || /^mailto:/i.test(link)) Qt.openUrlExternally(link)
        }
    }
}
