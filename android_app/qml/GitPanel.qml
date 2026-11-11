pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import TinaIde

ColumnLayout {
    id: panel
    required property FfiClientBridge bridge
    readonly property bool worktree: bridge.gitView === "worktree"
    readonly property bool loading: worktree ? bridge.gitStatusLoading : bridge.gitLogLoading
    readonly property string errorText: worktree ? bridge.gitStatusError : bridge.gitLogError
    readonly property var colors: ["#79b8ff", "#cba6f7", "#7ee2b8", "#f6c177", "#ee99bb", "#8bd5ed"]
    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        Label {
            Layout.fillWidth: true
            text: (panel.worktree ? "工作区 · " : "提交图 · ")
                  + (panel.bridge.gitRepository || "服务端默认仓库")
            color: "#e2ecff"
            elide: Text.ElideMiddle
        }
        Button {
            id: switchView
            text: panel.worktree ? "提交图" : "工作区"
            palette.buttonText: "#e2ecff"
            background: Rectangle {
                radius: 8
                color: switchView.down ? "#805378a8" : "#60304b70"
                border.color: "#588cb8ff"
            }
            onClicked: panel.bridge.handleGitCommand(panel.worktree ? "@GIT tree" : "@GIT worktree")
        }
        Button {
            id: refresh
            text: "刷新"
            enabled: !panel.loading
            palette.buttonText: "#e2ecff"
            background: Rectangle {
                radius: 8
                color: refresh.down ? "#805378a8" : "#60304b70"
                border.color: "#588cb8ff"
                opacity: refresh.enabled ? 1 : 0.5
            }
            onClicked: panel.bridge.handleGitCommand("@GIT refresh")
        }
    }
    Label {
        Layout.fillWidth: true
        visible: panel.errorText.length > 0
        text: "获取失败：" + panel.errorText + "；可输入 @GIT refresh 重试。已有内容为上次快照。"
        color: "#ffd18b"
        wrapMode: Text.Wrap
    }
    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Flickable {
            id: graph
            objectName: "gitGraphViewport"
            anchors.fill: parent
            visible: !panel.worktree
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            readonly property real stepX: 240
            readonly property real stepY: 154
            contentWidth: Math.max(width, panel.bridge.gitNodes.length * stepX)
            contentHeight: Math.max(height, panel.bridge.gitLanes * stepY + 12)
            ScrollBar.horizontal: ScrollBar {}
            ScrollBar.vertical: ScrollBar {}
            onContentXChanged: edgesCanvas.requestPaint()
            onContentYChanged: edgesCanvas.requestPaint()
            onWidthChanged: edgesCanvas.requestPaint()
            onHeightChanged: edgesCanvas.requestPaint()

            // 画布仅覆盖可见区域，避免 100 条提交生成超宽纹理超出 Android GPU 限制。
            Canvas {
                id: edgesCanvas
                x: graph.contentX
                y: graph.contentY
                width: graph.width
                height: graph.height
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    ctx.lineWidth = 2
                    for (const edge of panel.bridge.gitEdges) {
                        const x1 = edge.fromIndex * graph.stepX + 120 - graph.contentX
                        const x2 = edge.toIndex * graph.stepX + 120 - graph.contentX
                        if (x2 < 0 || x1 > width) continue
                        const y1 = edge.fromLane * graph.stepY + 22 - graph.contentY
                        const y2 = edge.toLane * graph.stepY + 22 - graph.contentY
                        ctx.strokeStyle = panel.colors[(edge.isMerge ? edge.toLane : edge.fromLane) % panel.colors.length]
                        ctx.beginPath()
                        ctx.moveTo(x1, y1)
                        // 在靠近父节点的一列弯向目标轨道，长边保留拓扑连接关系。
                        const bend = Math.max(x1 + 20, x2 - graph.stepX * 0.7)
                        ctx.lineTo(bend, y1)
                        ctx.bezierCurveTo(bend + 45, y1, x2 - 45, y2, x2, y2)
                        ctx.stroke()
                    }
                }
            }
            Connections {
                target: panel.bridge
                function onGitChanged() { edgesCanvas.requestPaint() }
            }
            Repeater {
                model: panel.bridge.gitNodes
                delegate: Item {
                    id: node
                    required property var modelData
                    objectName: "gitNode:" + modelData.hash
                    x: modelData.index * graph.stepX
                    y: modelData.lane * graph.stepY
                    width: graph.stepX
                    height: graph.stepY
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: 15
                        width: node.modelData.isHead ? 16 : 12
                        height: width
                        radius: width / 2
                        color: node.modelData.isHead ? "#34d399" : panel.colors[node.modelData.lane % panel.colors.length]
                        border.width: 2
                        border.color: "#eef6ff"
                    }
                    Rectangle {
                        x: 10
                        y: 40
                        width: parent.width - 20
                        height: 106
                        radius: 10
                        color: "#ca142238"
                        border.color: node.modelData.isHead ? "#34d399" : "#48799cc8"
                        Column {
                            anchors.fill: parent
                            anchors.margins: 9
                            spacing: 4
                            Text {
                                width: parent.width
                                text: node.modelData.refs || node.modelData.hash
                                color: node.modelData.isHead ? "#7ee2b8" : "#79b8ff"
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                text: node.modelData.subject
                                color: "#edf4ff"
                                font.pixelSize: 13
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                text: node.modelData.hash + " · " + node.modelData.author
                                color: "#b0c4de"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                            Text { text: node.modelData.time; color: "#8ea8c9"; font.pixelSize: 11 }
                        }
                        HoverHandler { id: hover }
                        ToolTip.visible: hover.hovered
                        ToolTip.text: node.modelData.hash + "\n" + node.modelData.refs + "\n" + node.modelData.subject
                    }
                }
            }
            Label {
                x: graph.contentX + Math.max(0, (graph.width - width) / 2)
                y: graph.contentY + (graph.height - height) / 2
                visible: !panel.loading && !panel.errorText && panel.bridge.gitNodes.length === 0
                text: "仓库暂无提交"
                color: "#b0c4de"
            }
        }
        ListView {
            id: statusList
            objectName: "gitStatusList"
            anchors.fill: parent
            visible: panel.worktree
            clip: true
            spacing: 4
            model: panel.bridge.gitStatus
            currentIndex: count > 0 ? 0 : -1
            keyNavigationEnabled: true
            ScrollBar.vertical: ScrollBar {}
            Keys.onReturnPressed: panel.bridge.openGitFileDiff(currentIndex)
            Keys.onEnterPressed: panel.bridge.openGitFileDiff(currentIndex)
            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                objectName: "gitStatus:" + modelData.path
                width: statusList.width
                height: 48
                radius: 8
                color: ListView.isCurrentItem ? "#60365d8c" : "#40172438"
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    Label {
                        text: row.modelData.status
                        font.family: "monospace"
                        color: row.modelData.status === "??" ? "#ffd18b" : "#7ee2b8"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: row.modelData.path
                        color: "#e2ecff"
                        elide: Text.ElideMiddle
                    }
                    Label { text: "查看差异"; color: "#8ea8c9" }
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        statusList.currentIndex = row.index
                        panel.bridge.openGitFileDiff(row.index)
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: !panel.loading && !panel.errorText && statusList.count === 0
                text: "工作区干净，没有修改"
                color: "#b0c4de"
            }
        }
        BusyIndicator {
            anchors.centerIn: parent
            width: 32
            height: width
            running: panel.loading
            visible: running
        }
    }
    Label {
        Layout.fillWidth: true
        text: panel.worktree ? "左列为暂存状态，右列为工作区状态 · 点击文件查看相对 HEAD 的差异"
                             : "最多 100 条提交 · 拖动查看分支与合并 · @GIT refresh 手动更新"
        color: "#8ea8c9"
        font.pixelSize: 11
        elide: Text.ElideRight
    }
}
