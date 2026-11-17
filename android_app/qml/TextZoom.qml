pragma ComponentBehavior: Bound
import QtQuick

PinchHandler {
    id: zoom
    required property Flickable viewport
    required property TextEdit textItem
    property real zoomFactor: 1
    readonly property real minimumZoom: 0.625
    readonly property real maximumZoom: 2.5
    property bool adjusting: false
    property real initialZoom: 1
    property real initialFontSize: 16
    property int anchorPosition: 0
    property point anchorOffset: Qt.point(0, 0)
    property point focalScenePosition: Qt.point(0, 0)
    signal zoomRequested(real factor)

    parent: viewport
    target: null
    minimumPointCount: 2
    maximumPointCount: 2
    acceptedDevices: PointerDevice.TouchScreen | PointerDevice.TouchPad
    grabPermissions: PointerHandler.CanTakeOverFromAnything
                     | PointerHandler.ApprovesTakeOverByAnything
    rotationAxis.enabled: false
    xAxis.enabled: false
    yAxis.enabled: false

    // 锚定双指之间的字符；字号改变后按新排版恢复位置，包括 Markdown 换行。
    function restoreAnchor() {
        const rect = textItem.positionToRectangle(anchorPosition)
        const mapped = textItem.mapToItem(viewport, rect.x, rect.y)
        const focal = viewport.mapFromItem(null, focalScenePosition.x, focalScenePosition.y)
        const ratio = textItem.font.pixelSize / initialFontSize
        viewport.contentX = Math.max(0, Math.min(viewport.contentWidth - viewport.width,
            viewport.contentX + mapped.x + anchorOffset.x * ratio - focal.x))
        viewport.contentY = Math.max(0, Math.min(viewport.contentHeight - viewport.height,
            viewport.contentY + mapped.y + anchorOffset.y * rect.height - focal.y))
        if (!active) adjusting = false
    }

    onActiveChanged: {
        if (active) {
            viewport.cancelFlick()
            adjusting = true
            initialZoom = zoomFactor
            initialFontSize = textItem.font.pixelSize
            const point = textItem.mapFromItem(null, centroid.scenePosition.x, centroid.scenePosition.y)
            anchorPosition = textItem.positionAt(point.x, point.y)
            const rect = textItem.positionToRectangle(anchorPosition)
            anchorOffset = Qt.point(point.x - rect.x, (point.y - rect.y) / Math.max(1, rect.height))
            focalScenePosition = centroid.scenePosition
        } else if (adjusting) {
            Qt.callLater(restoreAnchor)
        }
    }
    onScaleChanged: {
        if (!active) return
        focalScenePosition = centroid.scenePosition
        zoomRequested(Math.max(minimumZoom, Math.min(maximumZoom, initialZoom * activeScale)))
        Qt.callLater(restoreAnchor)
    }
}
