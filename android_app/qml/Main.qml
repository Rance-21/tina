pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import TinaIde

Window {
    id: mainWindow
    // 16:10 横屏比例 (1280 x 800)
    width: 1280
    height: 800
    minimumWidth: 960
    minimumHeight: 600
    visible: true
    title: qsTr("Tina Android IDE — Client Shell (16:10)")
    color: "#090d16"
    // 默认沿用现有服务端地址；桌面测试可从启动参数注入本地模拟服务端。
    property string serverHost: "100.65.173.4"
    property int serverPort: 3490

    FfiClientBridge {
        id: ffiBridge
        objectName: "workspaceBridge"
        onPingResult: function(ok, message) {
            mainWindow.title = ok ? "Tina — 已连接 Fedora" : "Tina — " + message
        }
        onDocumentsChanged: {
            // 增量追加，已有编辑器及其撤销栈不会随列表更新被重建。
            const opened = documents
            while (fileTabsModel.count < opened.length)
                fileTabsModel.append({document: opened[fileTabsModel.count]})
        }
        onFileOpened: {
            rootCanvas.popupMode = "none"
            Qt.callLater(rootCanvas.focusCurrentEditor)
        }
        onBrowserChanged: {
            fileList.currentIndex = entries.length > 0 ? 0 : -1
        }
    }

    Component.onCompleted: ffiBridge.connectToServer(serverHost, serverPort)
    Shortcut { sequence: "Ctrl+S"; onActivated: ffiBridge.saveCurrent() }
    Shortcut {
        sequence: "Ctrl+Tab"
        onActivated: ffiBridge.selectDocument((ffiBridge.activeIndex + 1) % Math.max(1, fileTabsModel.count))
    }
    Shortcut {
        sequence: "Ctrl+Shift+Tab"
        onActivated: ffiBridge.selectDocument((ffiBridge.activeIndex - 1 + fileTabsModel.count)
                                              % Math.max(1, fileTabsModel.count))
    }
    Shortcut { sequence: "Ctrl+P"; onActivated: rootCanvas.showFiles() }
    Shortcut {
        sequence: "Escape"
        enabled: rootCanvas.popupMode !== "none"
        onActivated: {
            rootCanvas.popupMode = "none"
            rootCanvas.focusCurrentEditor()
        }
    }

    Item {
        id: rootCanvas
        anchors.fill: parent

        // 状态控制
        readonly property int currentTabIndex: ffiBridge.activeIndex

        // 右侧 AI Agent 窗口：默认不显示，输入 @AI 回车切换显示/关闭（点击别处不关闭）
        property bool agentWindowVisible: false

        // 对齐底部对话框弹出的窗口状态：
        // "none"   -> 不显示
        // "menu"   -> 首字母输入 '@' 时展示的三行菜单 (AI / GIT / FILE)
        // "git"    -> 输入 @GIT 回车后，把当前窗口变成横向可滑动的 Git 树
        // "folder" -> 点击左侧 Folder 按钮，或输入 @FILE 回车后弹出的文件窗口
        property string popupMode: "none"

        // 布局核心几何参数：
        // 左侧标签栏缩短，贴紧屏幕左侧；逻辑上保留左右分界区域，视觉上去除代码区左右分割线
        // 左侧最低点与右侧 Agent 窗口最低点一致 (panelBottomY)
        readonly property real leftColumnWidth: 132
        readonly property real rightAgentWidth: Math.min(320, width * 0.3)
        readonly property real bottomInputHeight: 54
        readonly property real bottomMargin: 18
        readonly property real panelBottomY: height - bottomMargin - bottomInputHeight - 16

        ListModel { id: fileTabsModel }

        function focusCurrentEditor() {
            const pane = editors.itemAt(ffiBridge.activeIndex) as CodePane
            if (pane) pane.focusEditor()
        }
        function showFiles() {
            popupMode = "folder"
            ffiBridge.openFileBrowser()
            fileList.forceActiveFocus()
        }

        // @ 出来的三行菜单数据：左侧图标，往右字 (AI / GIT / FILE)
        ListModel {
            id: atMenuModel
            ListElement {
                cmdKey: "AI"
                iconSymbol: "✦"
                iconColor: "#58a6ff"
            }
            ListElement {
                cmdKey: "GIT"
                iconSymbol: "⎇"
                iconColor: "#f59e0b"
            }
            ListElement {
                cmdKey: "FILE"
                iconSymbol: "📁"
                iconColor: "#34d399"
            }
        }

        // 横向 Git 树占位数据
        ListModel {
            id: gitTreeCommitsModel
            ListElement {
                hash: "a1b2c3d"
                branch: "HEAD -> main"
                msg: "feat(ui): 16:10 glass shell & @ popup"
                author: "rance"
                isHead: true
            }
            ListElement {
                hash: "8f9e0d1"
                branch: "origin/main"
                msg: "feat(rpc): ide frame encode/decode"
                author: "rance"
                isHead: false
            }
            ListElement {
                hash: "5c4b3a2"
                branch: ""
                msg: "perf(net): epoll event loop thread pool"
                author: "rance"
                isHead: false
            }
            ListElement {
                hash: "2d1e0f9"
                branch: "tag: v0.1"
                msg: "init: tina C++23 server skeleton"
                author: "rance"
                isHead: false
            }
            ListElement {
                hash: "0a9b8c7"
                branch: ""
                msg: "docs(workbench): architecture & protocol"
                author: "rance"
                isHead: false
            }
        }

        // 右侧 AI Agent 窗口内的对话列表模型（左上角模型 Icon + 右边对话名，点击展开）
        ListModel {
            id: agentDialoguesModel
            ListElement {
                modelIcon: "✦"
                modelColor: "#58a6ff"
                modelTag: "Gemini"
                dialogueTitle: "重构 epoll 事件分发与零拷贝发包"
                expanded: true
                detailText: "已分析 src/net/epoll.cppm 与 socket.cppm：\n建议在发送大文件分块时引入 MSG_ZEROCOPY，并将 signalfd 纳入主 EventLoop 统一管理。"
            }
            ListElement {
                modelIcon: "◈"
                modelColor: "#c084fc"
                modelTag: "Codex"
                dialogueTitle: "生成 Rust FFI 与 C++23 QObject 桥接层"
                expanded: false
                detailText: "已生成 extern \"C\" 导出符号定义：\n- tina_ffi_client_create\n- tina_ffi_client_send_command\n- tina_ffi_client_send_agent_prompt"
            }
            ListElement {
                modelIcon: "❖"
                modelColor: "#34d399"
                modelTag: "Antigravity"
                dialogueTitle: "检查 workspace 相对路径安全校验"
                expanded: false
                detailText: "通过 std::filesystem::weakly_canonical 校验客户端请求路径，防止 .. 越界访问项目根目录之外的文件。"
            }
        }

        // ====================================================================
        // 0. 底层：16:10 暗色背景图 + 中间代码区域（代码区、左侧区域、未开启AI时的右侧区域均不模糊，且无视觉分割线）
        // ====================================================================
        Item {
            id: backgroundAndCodeScene
            anchors.fill: parent

            // 16:10 背景图片
            Image {
                id: wallpaperImage
                anchors.fill: parent
                source: "qrc:/qt/qml/TinaIde/assets/bg_16_10.png"
                fillMode: Image.PreserveAspectCrop
            }

            // 暗色底衬（不模糊，无分割线）
            Rectangle {
                anchors.fill: parent
                color: "#5c080c14"
            }

            // ----------------------------------------------------------------
            // 无边框代码区，底部避开命令栏；AI 隐藏时释放右侧空间。
            Item {
                id: centerCodeArea
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.bottomMargin: rootCanvas.bottomMargin + rootCanvas.bottomInputHeight + 18
                anchors.left: parent.left
                anchors.leftMargin: rootCanvas.leftColumnWidth + 14
                anchors.right: parent.right
                anchors.rightMargin: rootCanvas.agentWindowVisible ? rootCanvas.rightAgentWidth + 20 : 20
                Text {
                    anchors.centerIn: parent
                    visible: fileTabsModel.count === 0
                    text: "输入 @FILE 或按 Ctrl+P 打开项目文件"
                    color: "#a6bcda"
                    font.pixelSize: 17
                }
                Repeater {
                    id: editors
                    model: fileTabsModel
                    delegate: CodePane {
                        required property int index
                        anchors.fill: parent
                        bridge: ffiBridge
                        active: index === ffiBridge.activeIndex
                    }
                }
            }
        }

        // 点击别处关闭弹窗的遮罩层（仅关闭 menu / git / folder 弹窗，绝不关闭右侧 AI 窗口）
        // ====================================================================
        MouseArea {
            id: outsideClickCatcher
            anchors.fill: parent
            enabled: rootCanvas.popupMode !== "none"
            visible: rootCanvas.popupMode !== "none"
            onClicked: {
                rootCanvas.popupMode = "none"
            }
        }

        // ====================================================================
        // 1. 左侧：单文件标签页区
        //    - 区域本身不模糊、无分界线、底部无小横线
        //    - 每个标签缩短、紧贴屏幕左侧（左边取消圆角 flatLeftEdge: true，仅右边圆角）
        //    - 标签内部透明模糊，选中高亮
        //    - 区域逻辑最低点与右边 Agent 窗口最低点一致
        // ====================================================================
        Item {
            id: leftTabsContainer
            x: 0
            y: 18
            width: rootCanvas.leftColumnWidth
            height: rootCanvas.panelBottomY - y

            ListView {
                id: tabsColumn
                anchors.fill: parent
                spacing: 10
                clip: true
                model: fileTabsModel
                ScrollBar.vertical: ScrollBar {}
                delegate: Item {
                    id: tabDelegate
                    required property int index
                    required property WorkspaceDocument document
                    width: leftTabsContainer.width
                    height: 48
                    readonly property bool isSelected: index === ffiBridge.activeIndex
                    GlassPanel {
                        anchors.fill: parent
                        blurSource: backgroundAndCodeScene
                        cornerRadius: 14
                        flatLeftEdge: true
                        blurRadius: 24
                        highlightGlow: tabDelegate.isSelected || tabDelegate.document.dirty
                        tintColor: tabDelegate.isSelected ? "#98203a68" : "#58101726"
                        // 未保存标签边缘用琥珀色，与蓝色选中态区分。
                        borderColor: tabDelegate.document.dirty ? "#ffd18b"
                                     : tabDelegate.isSelected ? "#c06cb8ff" : "#348aa8d8"
                        borderWidth: tabDelegate.document.dirty ? 2 : 1
                    }
                    Rectangle {
                        width: 3
                        height: parent.height
                        color: tabDelegate.document.dirty ? "#ffd18b" : "#63b3ed"
                        visible: tabDelegate.isSelected || tabDelegate.document.dirty
                    }
                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 8
                        verticalAlignment: Text.AlignVCenter
                        text: (tabDelegate.document.dirty ? "• " : "") + tabDelegate.document.name
                        color: tabDelegate.isSelected ? "#ffffff" : "#b0c4de"
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            ffiBridge.selectDocument(tabDelegate.index)
                            rootCanvas.focusCurrentEditor()
                        }
                    }
                    ToolTip.visible: tabHover.hovered
                    ToolTip.text: tabDelegate.document.path
                    HoverHandler { id: tabHover }
                }
            }
        }

        // 2. 右侧：AI Agent 窗口
        //    - 平时不显示（右侧区域也不模糊、无分界线）
        //    - 只有输入 @AI 回车才展示，展示时带有左侧分界线与背景模糊
        //    - 点击别处不关闭，再次输入 @AI 回车关闭
        //    - 最低点与左侧标签区最低点一致
        // ====================================================================
        Item {
            id: rightAgentWindow
            width: rootCanvas.rightAgentWidth
            anchors.right: parent.right
            anchors.rightMargin: 16
            y: 18
            height: rootCanvas.panelBottomY - y
            visible: rootCanvas.agentWindowVisible
            opacity: rootCanvas.agentWindowVisible ? 1.0 : 0.0

            Behavior on opacity { NumberAnimation { duration: 180 } }

            // 展示右侧 AI 窗口时显示的左侧分界线
            Rectangle {
                id: agentLeftDividerLine
                x: -10
                y: 0
                width: 1.5
                height: parent.height
                color: "#527cb8ff"
            }

            // 阻止点击穿透到外层
            MouseArea {
                anchors.fill: parent
            }

            GlassPanel {
                anchors.fill: parent
                blurSource: backgroundAndCodeScene
                cornerRadius: 20
                blurRadius: 32
                tintColor: "#74111828"
                borderColor: "#4c8cb4ff"
                borderWidth: 1.2
                highlightGlow: true
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 12

                // 顶部标题栏
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Rectangle {
                        implicitWidth: 8
                        implicitHeight: 8
                        radius: 4
                        color: "#38bdf8"
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "AI Agent"
                        color: "#eef4ff"
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    Text {
                        text: "@AI"
                        color: "#7c93b8"
                        font.family: "monospace"
                        font.pixelSize: 12
                    }
                }

                // 内部一条条对话列表（左上角是模型的 Icon，从 Icon 右边开始是对话名，点击展开）
                ListView {
                    id: agentDialogueList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 10
                    clip: true
                    model: agentDialoguesModel

                    delegate: Item {
                        id: dialogueCard
                        required property var model
                        required property int index
                        width: agentDialogueList.width
                        height: cardCol.implicitHeight + 22

                        Rectangle {
                            anchors.fill: parent
                            radius: 14
                            color: cardMouse.containsMouse
                                   ? "#42243452"
                                   : (dialogueCard.model.expanded ? "#381e2d48" : "#26162034")
                            border.color: dialogueCard.model.expanded ? "#687cb8ff" : "#288aa8d8"
                            border.width: 1
                        }

                        ColumnLayout {
                            id: cardCol
                            anchors.top: parent.top
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.margins: 11
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                // 左上角模型 Icon
                                Rectangle {
                                    Layout.alignment: Qt.AlignTop
                                    implicitWidth: 30
                                    implicitHeight: 30
                                    radius: 15
                                    color: "#30ffffff"
                                    border.color: dialogueCard.model.modelColor
                                    border.width: 1.5

                                    Text {
                                        anchors.centerIn: parent
                                        text: dialogueCard.model.modelIcon
                                        color: dialogueCard.model.modelColor
                                        font.pixelSize: 15
                                        font.bold: true
                                    }
                                }

                                // 从 Icon 右边开始是对话名
                                Text {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    text: dialogueCard.model.dialogueTitle
                                    color: "#f0f6ff"
                                    font.pixelSize: 13
                                    font.weight: Font.Medium
                                    wrapMode: Text.WordWrap
                                }

                                Text {
                                    Layout.alignment: Qt.AlignTop
                                    text: dialogueCard.model.expanded ? "▾" : "▸"
                                    color: "#7c93b8"
                                    font.pixelSize: 13
                                }
                            }

                            Rectangle {
                                visible: dialogueCard.model.expanded
                                Layout.fillWidth: true
                                implicitHeight: 1
                                color: "#22ffffff"
                            }

                            Text {
                                visible: dialogueCard.model.expanded
                                Layout.fillWidth: true
                                text: "[" + dialogueCard.model.modelTag + "] " + dialogueCard.model.detailText
                                color: "#b6c8e4"
                                font.pixelSize: 12
                                lineHeight: 1.35
                                wrapMode: Text.WordWrap
                            }
                        }

                        MouseArea {
                            id: cardMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                agentDialoguesModel.setProperty(dialogueCard.index, "expanded", !dialogueCard.model.expanded)
                            }
                        }
                    }
                }
            }
        }

        // ====================================================================
        // 3. 对齐底部对话框弹出的模糊窗口（左右严格与 bottomDialogBar 对齐）
        //    支持三种状态：
        //    - "menu"   : 首字母输入 '@' 时出现的三行选项（左侧图标，往右字：AI / GIT / FILE）
        //    - "git"    : 输入 @GIT 回车后，把当前三行窗口变成横向可滑动（<- / ->）的 Git 树
        //    - "folder" : 点击 Folder 按钮或输入 @FILE 回车后弹出的文件列表
        //    以上弹窗均在点击别处时关闭。
        // ====================================================================
        Item {
            id: alignedPopupWindow
            // 左右严格与下方对话框（bottomDialogBar）对齐
            x: bottomDialogBar.x
            width: bottomDialogBar.width
            height: rootCanvas.popupMode === "menu" ? 180
                    : rootCanvas.popupMode === "folder" ? Math.min(380, bottomDialogBar.y - 24) : 190
            y: bottomDialogBar.y - height - 10
            visible: rootCanvas.popupMode !== "none"
            opacity: visible ? 1.0 : 0.0

            Behavior on opacity { NumberAnimation { duration: 140 } }
            Behavior on height { NumberAnimation { duration: 140 } }

            // 拦截弹窗内部点击，防止触发外层 outsideClickCatcher 关闭
            MouseArea {
                anchors.fill: parent
            }

            GlassPanel {
                anchors.fill: parent
                blurSource: backgroundAndCodeScene
                cornerRadius: 20
                blurRadius: 32
                tintColor: "#78121a2c"
                borderColor: "#588cb8ff"
                borderWidth: 1.2
                highlightGlow: true
            }

            // ----------------------------------------------------------------
            // 状态 A ("menu")：@ 出来的三行窗口（左侧图标，往右字：AI / GIT / FILE）
            // ----------------------------------------------------------------
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6
                visible: rootCanvas.popupMode === "menu"

                Repeater {
                    model: atMenuModel
                    delegate: Rectangle {
                        id: menuRow
                        required property var model
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 12
                        readonly property string currentFilter: cmdInput.text.length > 1
                                                                ? cmdInput.text.substring(1).trim().toUpperCase()
                                                                : ""
                        readonly property bool isMatched: currentFilter.length > 0 && menuRow.model.cmdKey.startsWith(currentFilter)
                        color: rowMouse.containsMouse
                               ? "#482a4268"
                               : (isMatched ? "#3622385a" : "transparent")
                        border.color: (rowMouse.containsMouse || isMatched) ? "#687cb8ff" : "transparent"
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 14
                            anchors.rightMargin: 14
                            spacing: 12

                            // 左侧图标
                            Rectangle {
                                implicitWidth: 28
                                implicitHeight: 28
                                radius: 8
                                color: "#28ffffff"
                                border.color: menuRow.model.iconColor
                                border.width: 1.2

                                Text {
                                    anchors.centerIn: parent
                                    text: menuRow.model.iconSymbol
                                    color: menuRow.model.iconColor
                                    font.pixelSize: 14
                                    font.bold: true
                                }
                            }

                            // 往右字：AI / GIT / FILE
                            Text {
                                Layout.fillWidth: true
                                text: menuRow.model.cmdKey
                                color: "#f0f6ff"
                                font.family: "monospace"
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                            }
                        }

                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                cmdInput.text = "@" + menuRow.model.cmdKey
                                rootCanvas.submitCurrentInput()
                            }
                        }
                    }
                }
            }

            // ----------------------------------------------------------------
            // 状态 B ("git")：把当前窗口变成横向 Git 树（支持 <- / -> 按钮及拖拽滑动）
            // ----------------------------------------------------------------
            RowLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 10
                visible: rootCanvas.popupMode === "git"

                // 左滑按钮 "<-"
                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: 38
                    implicitHeight: 38
                    radius: 19
                    color: leftSlideMouse.containsMouse ? "#4c2c446e" : "#2e1a2a44"
                    border.color: "#588cb8ff"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "<-"
                        color: "#e2ecff"
                        font.family: "monospace"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    MouseArea {
                        id: leftSlideMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            gitHorizFlick.contentX = Math.max(0, gitHorizFlick.contentX - 240)
                        }
                    }
                }

                // 中间横向可滑动的 Git 树占位图
                Flickable {
                    id: gitHorizFlick
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: gitNodesRow.implicitWidth + 24
                    contentHeight: height
                    flickableDirection: Flickable.HorizontalFlick
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true

                    Behavior on contentX { NumberAnimation { duration: 180; easing.type: Easing.OutQuad } }

                    Row {
                        id: gitNodesRow
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 0

                        Repeater {
                            model: gitTreeCommitsModel
                            delegate: Item {
                                id: commitNode
                                required property var model
                                required property int index
                                width: 230
                                height: 136

                                // 连接主干横线（横着的 Git 树连线）
                                Rectangle {
                                    visible: commitNode.index < gitTreeCommitsModel.count - 1
                                    x: 115
                                    y: 40
                                    width: 230
                                    height: 3
                                    color: "#6858a6ff"
                                }

                                // 分支示意曲线（第2个节点画一条分叉演进线作为占位示意）
                                Canvas {
                                    visible: commitNode.index === 1
                                    anchors.fill: parent
                                    onPaint: {
                                        var ctx = getContext("2d")
                                        ctx.clearRect(0, 0, width, height)
                                        ctx.strokeStyle = "#c084fc"
                                        ctx.lineWidth = 2
                                        ctx.beginPath()
                                        ctx.moveTo(115, 41)
                                        ctx.bezierCurveTo(150, 41, 160, 18, 200, 18)
                                        ctx.stroke()
                                    }
                                }

                                Column {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    y: 8
                                    width: 206
                                    spacing: 8

                                    // 分支标签
                                    Rectangle {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        height: 20
                                        width: branchLabel.implicitWidth + 14
                                        radius: 10
                                        visible: commitNode.model.branch.length > 0
                                        color: commitNode.model.isHead ? "#3834d399" : "#3258a6ff"
                                        border.color: commitNode.model.isHead ? "#34d399" : "#58a6ff"
                                        border.width: 1

                                        Text {
                                            id: branchLabel
                                            anchors.centerIn: parent
                                            text: commitNode.model.branch
                                            color: "#f0f6ff"
                                            font.family: "monospace"
                                            font.pixelSize: 11
                                        }
                                    }

                                    Item {
                                        visible: commitNode.model.branch.length === 0
                                        width: 1
                                        height: 20
                                    }

                                    // Commit 圆点节点
                                    Rectangle {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        width: 18
                                        height: 18
                                        radius: 9
                                        color: commitNode.model.isHead ? "#34d399" : "#58a6ff"
                                        border.color: "#ffffff"
                                        border.width: 2.5
                                    }

                                    // Commit 信息卡片
                                    Rectangle {
                                        width: parent.width
                                        height: 62
                                        radius: 10
                                        color: "#3218243a"
                                        border.color: "#388aa8d8"
                                        border.width: 1

                                        Column {
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            spacing: 4

                                            RowLayout {
                                                width: parent.width
                                                Text {
                                                    text: commitNode.model.hash
                                                    color: "#7cc4ff"
                                                    font.family: "monospace"
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                }
                                                Item { Layout.fillWidth: true }
                                                Text {
                                                    text: commitNode.model.author
                                                    color: "#8aa0c0"
                                                    font.pixelSize: 11
                                                }
                                            }

                                            Text {
                                                width: parent.width
                                                text: commitNode.model.msg
                                                color: "#e2ecff"
                                                font.pixelSize: 12
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // 右滑按钮 "->"
                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: 38
                    implicitHeight: 38
                    radius: 19
                    color: rightSlideMouse.containsMouse ? "#4c2c446e" : "#2e1a2a44"
                    border.color: "#588cb8ff"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "->"
                        color: "#e2ecff"
                        font.family: "monospace"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    MouseArea {
                        id: rightSlideMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            var maxScroll = Math.max(0, gitHorizFlick.contentWidth - gitHorizFlick.width)
                            gitHorizFlick.contentX = Math.min(maxScroll, gitHorizFlick.contentX + 240)
                        }
                    }
                }
            }

            // ----------------------------------------------------------------
            // 状态 C：复用 AI / GIT / FILE 所在的同一个毛玻璃弹窗。
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 4
                visible: rootCanvas.popupMode === "folder"
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        text: "↑ 上级"
                        enabled: ffiBridge.directory.length > 0 && !ffiBridge.browsing
                        onClicked: ffiBridge.parentDirectory()
                    }
                    Label {
                        Layout.fillWidth: true
                        text: ffiBridge.directory.length > 0 ? ffiBridge.directory : "项目"
                        color: "#dceaff"
                        elide: Text.ElideMiddle
                    }
                    BusyIndicator {
                        running: ffiBridge.browsing
                        visible: running
                        implicitWidth: 30
                        implicitHeight: 30
                    }
                    Button {
                        text: "刷新"
                        enabled: !ffiBridge.browsing
                        onClicked: ffiBridge.openFileBrowser()
                    }
                }
                ListView {
                    id: fileList
                    objectName: "fileBrowserList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: ffiBridge.entries
                    currentIndex: -1
                    keyNavigationEnabled: true
                    keyNavigationWraps: true
                    highlightMoveDuration: 100
                    ScrollBar.vertical: ScrollBar {}
                    Keys.onReturnPressed: ffiBridge.activateEntry(currentIndex)
                    Keys.onEnterPressed: ffiBridge.activateEntry(currentIndex)
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Up && (event.modifiers & Qt.AltModifier)) {
                            ffiBridge.parentDirectory()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Backspace) {
                            ffiBridge.parentDirectory()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Right) {
                            ffiBridge.activateEntry(currentIndex)
                            event.accepted = true
                        }
                    }
                    delegate: Rectangle {
                        id: fileRow
                        required property var modelData
                        required property int index
                        width: fileList.width
                        height: 48
                        radius: 10
                        color: ListView.isCurrentItem ? "#60365d8c" : "#101d2f48"
                        border.color: ListView.isCurrentItem ? "#80baff" : "transparent"
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 14
                            anchors.rightMargin: 14
                            Label {
                                text: fileRow.modelData.type === 1 ? "▸" : fileRow.modelData.type === 2 ? "↗" : "≡"
                                color: fileRow.modelData.type === 1 ? "#ffd18b" : "#8fc2ff"
                                font.pixelSize: 22
                            }
                            Label {
                                Layout.fillWidth: true
                                text: fileRow.modelData.name
                                color: "#edf4ff"
                                elide: Text.ElideRight
                            }
                            Label {
                                text: fileRow.modelData.type === 1 ? "目录" : fileRow.modelData.type === 2 ? "符号链接" : "文件"
                                color: "#8ea8c9"
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                fileList.currentIndex = fileRow.index
                                fileList.forceActiveFocus()
                                ffiBridge.activateEntry(fileRow.index)
                            }
                        }
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: !ffiBridge.browsing && fileList.count === 0
                        text: "没有可显示的项目或文件；可点击刷新重新获取"
                        color: "#a6bcda"
                    }
                }
            }
        }

        // 4. 下边：统一对话框（删除最左侧的 $，以 Folder 按钮取代；右边是向上箭头提交）
        // ====================================================================
        Item {
            id: bottomDialogBar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            anchors.bottomMargin: rootCanvas.bottomMargin
            height: rootCanvas.bottomInputHeight

            GlassPanel {
                anchors.fill: parent
                blurSource: backgroundAndCodeScene
                cornerRadius: height / 2
                blurRadius: 52
                highlightGlow: cmdInput.activeFocus
                tintColor: cmdInput.activeFocus ? "#82141e32" : "#68101726"
                borderColor: cmdInput.activeFocus ? "#886ab4ff" : "#408aa8d8"
                borderWidth: cmdInput.activeFocus ? 1.4 : 1.1
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 7
                anchors.rightMargin: 7
                spacing: 10

                // 对话框内最左侧：Folder 按钮（取代原来的 $）
                Item {
                    id: folderButton
                    Layout.preferredWidth: rootCanvas.bottomInputHeight - 6
                    Layout.preferredHeight: rootCanvas.bottomInputHeight - 6

                    Rectangle {
                        anchors.fill: parent
                        radius: height / 2
                        color: rootCanvas.popupMode === "folder"
                               ? "#483b82f6"
                               : (folderMouse.containsMouse ? "#32ffffff" : "#1cffffff")
                        border.color: rootCanvas.popupMode === "folder" ? "#7cc4ff" : "#388aa8d8"
                        border.width: 1.1
                    }

                    // Folder 矢量图标
                    Canvas {
                        anchors.centerIn: parent
                        width: 20
                        height: 16
                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            ctx.strokeStyle = rootCanvas.popupMode === "folder" ? "#ffffff" : "#d0e0ff"
                            ctx.lineWidth = 1.7
                            ctx.lineJoin = "round"
                            ctx.beginPath()
                            ctx.moveTo(1.5, 4.0)
                            ctx.lineTo(1.5, 14.0)
                            ctx.lineTo(18.5, 14.0)
                            ctx.lineTo(18.5, 6.0)
                            ctx.lineTo(10.0, 6.0)
                            ctx.lineTo(7.8, 2.8)
                            ctx.lineTo(1.5, 2.8)
                            ctx.closePath()
                            ctx.stroke()
                        }
                    }

                    MouseArea {
                        id: folderMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (rootCanvas.popupMode === "folder") {
                                rootCanvas.popupMode = "none"
                            } else {
                                rootCanvas.showFiles()
                            }
                        }
                    }
                }

                // 中间命令行 / @ 输入框
                TextField {
                    id: cmdInput
                    objectName: "commandInput"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    verticalAlignment: TextInput.AlignVCenter
                    color: "#f0f6ff"
                    selectionColor: "#4058a6ff"
                    selectedTextColor: "#ffffff"
                    font.family: "monospace"
                    font.pixelSize: 14
                    placeholderText: qsTr("输入命令行，或输入 @ 呼出 AI / GIT / FILE...")
                    placeholderTextColor: "#6e84a6"
                    background: Item {}

                    onTextChanged: {
                        if (text.startsWith("@")) {
                            rootCanvas.popupMode = "menu"
                        } else if (rootCanvas.popupMode === "menu") {
                            rootCanvas.popupMode = "none"
                        }
                    }

                    Keys.onReturnPressed: rootCanvas.submitCurrentInput()
                    Keys.onEnterPressed: rootCanvas.submitCurrentInput()
                    Keys.onDownPressed: {
                        if (rootCanvas.popupMode === "folder") fileList.forceActiveFocus()
                    }
                }

                // 右边：向上箭头提交按钮
                Item {
                    id: submitButton
                    Layout.preferredWidth: rootCanvas.bottomInputHeight - 6
                    Layout.preferredHeight: rootCanvas.bottomInputHeight - 6

                    Rectangle {
                        anchors.fill: parent
                        radius: height / 2
                        color: submitMouse.pressed
                               ? "#2563eb"
                               : (submitMouse.containsMouse ? "#3b82f6" : "#38284878")
                        border.color: "#788cc0ff"
                        border.width: 1.2
                    }

                    Canvas {
                        anchors.centerIn: parent
                        width: 18
                        height: 18
                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            ctx.strokeStyle = "#f4f8ff"
                            ctx.lineWidth = 2.2
                            ctx.lineCap = "round"
                            ctx.lineJoin = "round"
                            ctx.beginPath()
                            ctx.moveTo(9, 15)
                            ctx.lineTo(9, 3.5)
                            ctx.moveTo(4, 8.5)
                            ctx.lineTo(9, 3.5)
                            ctx.lineTo(14, 8.5)
                            ctx.stroke()
                        }
                    }

                    MouseArea {
                        id: submitMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: rootCanvas.submitCurrentInput()
                    }
                }
            }
        }

        // ====================================================================
        Label {
            anchors.left: bottomDialogBar.left
            anchors.right: bottomDialogBar.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            text: ffiBridge.message
            color: ffiBridge.connected ? "#aac5e8" : "#ffd18b"
            font.pixelSize: 11
            elide: Text.ElideRight
        }

        // 提交处理逻辑：
        // - @AI 回车   -> 切换右侧 AI 窗口显示/关闭（展示时带分界线与模糊，点击别处不关闭，再次 @AI 关闭）
        // - @FILE 回车 -> 等同于点击左侧 Folder 按钮，打开当前目录文件列表（点击别处关闭）
        // - @GIT 回车  -> 把当前三行窗口变成横向可滑动的 Git 树（点击别处关闭）
        // - 其他 @...  -> 关闭三行弹窗并提交给 Agent
        // - 非 @ 开头  -> 作为命令行提交
        // ====================================================================
        function submitCurrentInput() {
            const raw = cmdInput.text.trim()
            if (raw.length === 0) {
                return
            }

            if (raw.startsWith("@")) {
                const upper = raw.toUpperCase()
                ffiBridge.submitAgentInput(raw)

                if (upper === "@AI") {
                    rootCanvas.agentWindowVisible = !rootCanvas.agentWindowVisible
                    cmdInput.text = ""
                    rootCanvas.popupMode = "none"
                } else if (upper === "@FILE") {
                    cmdInput.text = ""
                    rootCanvas.showFiles()
                } else if (upper === "@GIT") {
                    cmdInput.text = ""
                    rootCanvas.popupMode = "git"
                } else {
                    const titleText = raw.substring(1).trim()
                    if (titleText.length > 0) {
                        agentDialoguesModel.insert(0, {
                            modelIcon: "✦",
                            modelColor: "#58a6ff",
                            modelTag: "Agent",
                            dialogueTitle: titleText,
                            expanded: false,
                            detailText: "等待后端 FFI / RPC Agent 服务响应..."
                        })
                    }
                    cmdInput.text = ""
                    rootCanvas.popupMode = "none"
                }
            } else {
                ffiBridge.submitCommand(raw)
                cmdInput.text = ""
                rootCanvas.popupMode = "none"
            }
        }
    }
}
