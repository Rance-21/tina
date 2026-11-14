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
    // Qt 的 Android 全屏模式会同时隐藏状态栏和系统导航栏。
    visibility: Qt.platform.os === "android" ? Window.FullScreen : Window.Windowed
    title: qsTr("Tina Android IDE — Client Shell (16:10)")
    color: "#090d16"
    // 由 C++ 注入本地构建配置，QML 不保存真实域名；测试可注入本机模拟服务端。
    required property string serverHost
    required property int serverPort

    FfiClientBridge {
        id: ffiBridge
        objectName: "workspaceBridge"
        onPingResult: function(ok, message) {
            mainWindow.title = ok ? "Tina — 已连接 Fedora" : "Tina — " + message
        }
        onDocumentsChanged: {
            // 增量追加，已有编辑器及其撤销栈不会随列表更新被重建。
            const opened = documents
            while (editorDocuments.count < opened.length)
                editorDocuments.append({document: opened[editorDocuments.count]})
        }
        onFileOpened: {
            rootCanvas.popupMode = "none"
            Qt.callLater(rootCanvas.focusCurrentEditor)
        }
        onBrowserChanged: fileList.updateEntries(entries)
        onActiveTabChanged: Qt.callLater(rootCanvas.focusCurrentEditor)
    }

    Component.onCompleted: ffiBridge.connectToServer(serverHost, serverPort)
    Shortcut { sequence: "Ctrl+S"; onActivated: ffiBridge.saveCurrent() }
    Shortcut {
        sequence: "Ctrl+Tab"
        onActivated: ffiBridge.selectTab((ffiBridge.activeTabIndex + 1) % Math.max(1, ffiBridge.tabs.length))
    }
    Shortcut {
        sequence: "Ctrl+Shift+Tab"
        onActivated: ffiBridge.selectTab((ffiBridge.activeTabIndex - 1 + ffiBridge.tabs.length)
                                         % Math.max(1, ffiBridge.tabs.length))
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

        // 对齐底部对话框弹出的窗口状态：
        // "none"   -> 不显示
        // "menu"   -> 首字母输入 '@' 时展示的三行菜单 (GIT / FILE / TERMINAL)
        // "git"    -> @GIT tree / worktree 在同一个弹窗显示 Git 数据
        // "folder" -> 点击左侧 Folder 按钮，或输入 @FILE 回车后弹出的文件窗口
        property string popupMode: "none"

        // 布局核心几何参数：
        // 左侧标签栏缩短，贴紧屏幕左侧；逻辑上保留左右分界区域，视觉上去除代码区左右分割线
        // 标签栏底边跟随输入框，窗口尺寸变化时保持对齐。
        readonly property real leftColumnWidth: 108
        readonly property real codeLeftMargin: leftColumnWidth + 10
        readonly property real codeRightMargin: 20
        readonly property real bottomMargin: 18

        ListModel { id: editorDocuments }

        function focusCurrentEditor() {
            if (ffiBridge.terminalActive) {
                bottomDialogBar.focusInput()
                return
            }
            const pane = editors.itemAt(ffiBridge.activeIndex) as CodePane
            if (pane) pane.focusEditor()
        }
        function showFiles(projectPicker = false) {
            popupMode = "folder"
            if (projectPicker)
                ffiBridge.listProjects()
            else
                ffiBridge.openFileBrowser()
            fileList.forceActiveFocus()
        }

        // @ 出来的菜单数据：左侧图标，往右字 (GIT / FILE / TERMINAL)
        ListModel {
            id: atMenuModel
            ListElement {
                cmdKey: "GIT"
                iconName: "branch"
                iconColor: "#f59e0b"
            }
            ListElement {
                cmdKey: "FILE"
                iconName: "folder"
                iconColor: "#34d399"
            }
            ListElement {
                cmdKey: "TERMINAL"
                iconName: "terminal"
                iconColor: "#a5b4fc"
            }
        }

        // ====================================================================
        // 0. 底层：16:10 暗色背景图 + 中间代码区域（背景与代码区保持清晰，无视觉分割线）
        // ====================================================================
        Item {
            id: backgroundAndCodeScene
            anchors.fill: parent

            // 保持原图比例铺满屏幕。
            Image {
                id: wallpaperImage
                anchors.fill: parent
                source: "qrc:/qt/qml/TinaIde/assets/background.jpg"
                fillMode: Image.PreserveAspectCrop
            }

            // 压低壁纸亮度，让浅色代码在原图的高光区域也能读清。
            Rectangle {
                anchors.fill: parent
                color: "#99080c14"
            }

            // ----------------------------------------------------------------
            // 代码区铺到屏幕底边，输入框悬浮在代码上方。
            Item {
                id: centerCodeArea
                objectName: "centerCodeArea"
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.leftMargin: rootCanvas.codeLeftMargin
                anchors.right: parent.right
                anchors.rightMargin: rootCanvas.codeRightMargin
                Repeater {
                    id: editors
                    model: editorDocuments
                    delegate: CodePane {
                        required property int index
                        anchors.fill: parent
                        bridge: ffiBridge
                        active: index === ffiBridge.activeIndex
                        bottomOverlayHeight: rootCanvas.bottomMargin + bottomDialogBar.height
                    }
                }
            }
        }

        // 点击别处关闭弹窗的遮罩层（关闭 menu / git / folder 弹窗）
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
        // 1. 左侧：按打开顺序混排文件和终端标签
        //    - 区域本身不模糊、无分界线、底部无小横线
        //    - 每个标签缩短、紧贴屏幕左侧（左边取消圆角 flatLeftEdge: true，仅右边圆角）
        //    - 标签内部透明模糊，选中高亮
        //    - 区域底边与底部输入框对齐
        // ====================================================================
        Item {
            id: leftTabsContainer
            objectName: "fileTabs"
            x: 0
            anchors.top: parent.top
            anchors.topMargin: 18
            anchors.bottom: bottomDialogBar.bottom
            width: rootCanvas.leftColumnWidth

            ListView {
                id: tabsColumn
                anchors.fill: parent
                spacing: 10
                clip: true
                model: ffiBridge.tabs
                currentIndex: ffiBridge.activeTabIndex
                ScrollBar.vertical: ScrollBar {}
                delegate: Item {
                    id: tabDelegate
                    required property int index
                    required property var modelData
                    readonly property WorkspaceDocument document: modelData as WorkspaceDocument
                    readonly property TerminalSession terminal: modelData as TerminalSession
                    readonly property bool dirty: document !== null && document.dirty
                    readonly property bool isSelected: index === ffiBridge.activeTabIndex
                    objectName: terminal ? "terminalTab:" + terminal.sessionId : "fileTab:" + document.path
                    width: leftTabsContainer.width
                    height: 48
                    GlassPanel {
                        anchors.fill: parent
                        blurSource: backgroundAndCodeScene
                        cornerRadius: 14
                        flatLeftEdge: true
                        blurRadius: 24
                        highlightGlow: tabDelegate.isSelected || tabDelegate.dirty
                        tintColor: tabDelegate.isSelected ? "#98203a68" : "#58101726"
                        // 未保存标签边缘用琥珀色，与蓝色选中态区分。
                        borderColor: tabDelegate.dirty ? "#ffd18b"
                                     : tabDelegate.isSelected ? "#c06cb8ff" : "#348aa8d8"
                        borderWidth: tabDelegate.dirty ? 2 : 1
                    }
                    Rectangle {
                        width: 3
                        height: parent.height
                        color: tabDelegate.dirty ? "#ffd18b" : "#63b3ed"
                        visible: tabDelegate.isSelected || tabDelegate.dirty
                    }
                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: tabDelegate.terminal ? 30 : 10
                        anchors.rightMargin: 8
                        verticalAlignment: Text.AlignVCenter
                        text: tabDelegate.terminal ? tabDelegate.terminal.title
                              : (tabDelegate.dirty ? "• " : "") + tabDelegate.document.name
                        color: tabDelegate.isSelected ? "#ffffff" : "#b0c4de"
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }
                    UiIcon {
                        visible: tabDelegate.terminal !== null
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        height: 14
                        name: "terminal"
                        color: tabDelegate.terminal && tabDelegate.terminal.busy ? "#ffd18b" : "#a5b4fc"
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            ffiBridge.selectTab(tabDelegate.index)
                            rootCanvas.focusCurrentEditor()
                        }
                    }
                    ToolTip.visible: tabHover.hovered
                    ToolTip.text: tabDelegate.terminal ? tabDelegate.terminal.title
                                  + (tabDelegate.terminal.busy ? " · 执行中" : "") : tabDelegate.document.path
                    HoverHandler { id: tabHover }
                }
            }
        }

        // 2. 与底部输入框对齐的毛玻璃弹窗。
        //    支持三种状态：
        //    - "menu"   : 首字母输入 '@' 时出现的三行选项（GIT / FILE / TERMINAL）
        //    - "git"    : 显示 Git 提交图或工作区修改，均由显式命令请求服务端
        //    - "folder" : 点击 Folder 按钮或输入 @FILE 回车后弹出的文件列表
        //    以上弹窗均在点击别处时关闭。
        // ====================================================================
        Item {
            id: alignedPopupWindow
            objectName: "commandMenuPopup"
            // 左右严格与下方对话框（bottomDialogBar）对齐
            x: bottomDialogBar.x
            width: bottomDialogBar.width
            height: rootCanvas.popupMode === "menu" ? 12 * 2 + atMenuModel.count * 48 + (atMenuModel.count - 1) * 6
                    : Math.min(420, bottomDialogBar.y - 24)
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
                sampleOrigin: Qt.point(alignedPopupWindow.x, alignedPopupWindow.y)
                cornerRadius: 20
                blurRadius: 32
                tintColor: "#98121a2c"
                borderColor: "#588cb8ff"
                borderWidth: 1.2
                highlightGlow: true
            }

            // ----------------------------------------------------------------
            // 状态 A ("menu")：@ 菜单（GIT / FILE / TERMINAL）
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
                        objectName: "commandMenu:" + model.cmdKey
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 12
                        readonly property string currentFilter: bottomDialogBar.inputText.length > 1
                                                                ? bottomDialogBar.inputText.substring(1).trim().split(/\s+/)[0].toUpperCase()
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

                                UiIcon {
                                    anchors.centerIn: parent
                                    name: menuRow.model.iconName
                                    color: menuRow.model.iconColor
                                    width: 18
                                    height: 18
                                }
                            }

                            // 往右字：GIT / FILE / TERMINAL
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
                                if (menuRow.model.cmdKey === "TERMINAL") {
                                    bottomDialogBar.inputText = "@terminal "
                                    bottomDialogBar.focusInput()
                                } else {
                                    bottomDialogBar.inputText = "@" + menuRow.model.cmdKey
                                    rootCanvas.submitCurrentInput()
                                }
                            }
                        }
                    }
                }
            }

            // ----------------------------------------------------------------
            // Git 仍使用 @ 菜单弹出的这个窗口，展示真实提交图或工作区状态。
            GitPanel {
                anchors.fill: parent
                anchors.margins: 14
                visible: rootCanvas.popupMode === "git"
                bridge: ffiBridge
            }

            // ----------------------------------------------------------------
            // 状态 C：项目列表和项目内文件树共用同一个弹窗。
            ListView {
                id: fileList
                objectName: "fileBrowserList"
                anchors.fill: parent
                anchors.margins: 12
                visible: rootCanvas.popupMode === "folder"
                clip: true
                model: []
                currentIndex: -1
                keyNavigationEnabled: true
                keyNavigationWraps: true
                property string revealingPrefix: ""
                property real revealProgress: 1
                ScrollBar.vertical: ScrollBar {}

                NumberAnimation {
                    id: revealChildren
                    target: fileList
                    property: "revealProgress"
                    from: 0.35
                    to: 1
                    duration: 135
                    easing.type: Easing.OutCubic
                    onFinished: fileList.revealingPrefix = ""
                }

                function updateEntries(rows) {
                    // 更新分支后按路径恢复选中项，并保留当前滚动位置。
                    const previousIndex = currentIndex
                    const selectedPath = model[previousIndex]?.path
                    const scrollOffset = contentY - originY
                    // 找出原来没有可见子项、现在插入了子项的目录，只动画这一支。
                    if (visible && rows.length > model.length) {
                        let first = 0
                        while (first < model.length && model[first].path === rows[first].path)
                            ++first
                        const folder = rows[first - 1]
                        if (folder?.expanded && rows[first].depth > folder.depth
                                && !(model[first]?.depth > folder.depth)) {
                            revealingPrefix = folder.path + "/"
                            revealChildren.restart()
                        }
                    }
                    model = rows
                    const index = rows.findIndex(row => row.path === selectedPath)
                    currentIndex = index >= 0 ? index : Math.min(Math.max(previousIndex, 0), rows.length - 1)
                    if (index >= 0) {
                        forceLayout()
                        contentY = originY + Math.max(0, Math.min(scrollOffset, contentHeight - height))
                    }
                }

                Keys.onReturnPressed: ffiBridge.activateEntry(currentIndex)
                Keys.onEnterPressed: ffiBridge.activateEntry(currentIndex)
                Keys.onLeftPressed: {
                    const row = model[currentIndex]
                    if (row?.expanded) {
                        ffiBridge.setEntryExpanded(currentIndex, false)
                    } else if (row?.depth > 0) {
                        const parentPath = row.path.slice(0, row.path.lastIndexOf("/"))
                        currentIndex = model.findIndex(item => item.path === parentPath)
                    }
                }
                Keys.onRightPressed: {
                    const row = model[currentIndex]
                    if (row?.type !== 1)
                        return
                    if (!row.expanded)
                        ffiBridge.setEntryExpanded(currentIndex, true)
                    else if (model[currentIndex + 1]?.depth > row.depth)
                        incrementCurrentIndex()
                }
                Keys.onPressed: function(event) {
                    if ((event.key === Qt.Key_Up && (event.modifiers & Qt.AltModifier))
                        || event.key === Qt.Key_Backspace) {
                        if (ffiBridge.directory.length > 0)
                            ffiBridge.listProjects()
                        event.accepted = true
                    }
                }

                delegate: Rectangle {
                    id: fileRow
                    required property var modelData
                    required property int index
                    objectName: "fileRow:" + modelData.path
                    width: fileList.width
                    height: 48
                    radius: 10
                    color: ListView.isCurrentItem ? "#60365d8c" : "#101d2f48"
                    border.color: ListView.isCurrentItem ? "#80baff" : "transparent"
                    // 所有新行共用一个进度值，位置变化不参与列表布局。
                    opacity: fileList.revealingPrefix && modelData.path.startsWith(fileList.revealingPrefix)
                             ? fileList.revealProgress : 1
                    transform: Translate { y: -5 * (1 - fileRow.opacity) }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14 + Math.min(fileRow.modelData.depth * 20, fileRow.width / 2)
                        anchors.rightMargin: 14
                        UiIcon {
                            name: fileRow.modelData.type === 1
                                  ? (fileRow.modelData.loading ? "more" : fileRow.modelData.expanded ? "chevron-down" : "chevron-right")
                                  : (fileRow.modelData.type === 2 ? "link" : "file")
                            color: fileRow.modelData.type === 1 ? "#ffd18b" : "#8fc2ff"
                            implicitWidth: 22
                            implicitHeight: 22
                        }
                        Label {
                            Layout.fillWidth: true
                            text: fileRow.modelData.name
                            color: "#edf4ff"
                            font.pixelSize: 15
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

                BusyIndicator {
                    anchors.centerIn: parent
                    width: 30
                    height: width
                    running: fileList.visible && ffiBridge.browsing && fileList.count === 0
                    visible: running
                }
                Label {
                    anchors.centerIn: parent
                    visible: !ffiBridge.browsing && fileList.count === 0
                    text: "没有可显示的项目或文件"
                    color: "#a6bcda"
                }
            }
        }

        // 底部对话框直接作为终端；顶部手柄可向上拖高。
        TerminalPanel {
            id: bottomDialogBar
            objectName: "bottomDialogBar"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: rootCanvas.codeLeftMargin
            anchors.rightMargin: rootCanvas.codeRightMargin
            anchors.bottomMargin: rootCanvas.bottomMargin
            bridge: ffiBridge
            blurSource: backgroundAndCodeScene
            folderOpen: rootCanvas.popupMode === "folder"
            onFolderRequested: function(projects) {
                if (!projects && rootCanvas.popupMode === "folder")
                    rootCanvas.popupMode = "none"
                else
                    rootCanvas.showFiles(projects)
            }
            onSubmitted: rootCanvas.submitCurrentInput()
            onInputTextChanged: {
                commandError = ""
                if (inputText.startsWith("@")) rootCanvas.popupMode = "menu"
                else if (rootCanvas.popupMode === "menu") rootCanvas.popupMode = "none"
            }
        }

        // 提交处理逻辑：
        // - @FILE 回车 -> 等同于点击左侧 Folder 按钮，打开项目列表或当前项目文件树（点击别处关闭）
        // - @GIT tree / worktree / refresh -> Git 弹窗；diff [分支1] [分支2] -> 只读代码标签
        // - @TERMINAL add [id] / del id -> 管理左侧终端标签
        // - 其他 @...  -> 提示支持的命令，保留输入便于修改
        // - 非 @ 开头  -> 作为命令行提交
        // ====================================================================
        function submitCurrentInput() {
            const raw = bottomDialogBar.inputText
            if (raw.trim().startsWith("@")) {
                const upper = raw.trim().toUpperCase()
                if (upper.split(/\s+/)[0] === "@TERMINAL") {
                    // 先清掉来源草稿，避免新增/删除后误清另一个终端的输入。
                    bottomDialogBar.inputText = ""
                    if (!ffiBridge.handleTerminalCommand(raw)) {
                        bottomDialogBar.inputText = raw
                        bottomDialogBar.commandError = ffiBridge.message
                    }
                    rootCanvas.popupMode = "none"
                    Qt.callLater(bottomDialogBar.focusInput)
                    return
                }
                if (upper.split(/\s+/)[0] === "@GIT") {
                    // Git 输入走对应 RPC；分支名称保持原始大小写。
                    if (ffiBridge.handleGitCommand(raw)) {
                        bottomDialogBar.inputText = ""
                        const action = upper.split(/\s+/)[1] || "TREE"
                        rootCanvas.popupMode = action === "DIFF" ? "none" : "git"
                    } else {
                        bottomDialogBar.commandError = ffiBridge.message
                    }
                    return
                }
                if (upper === "@FILE") {
                    bottomDialogBar.inputText = ""
                    rootCanvas.showFiles()
                } else {
                    // 未知命令保留原始草稿，不创建占位内容，也不发送到终端。
                    bottomDialogBar.commandError = "未知命令，请使用 @GIT、@FILE 或 @TERMINAL"
                    rootCanvas.popupMode = "none"
                }
            } else {
                if (ffiBridge.submitCommand(raw))
                    bottomDialogBar.inputText = ""
                rootCanvas.popupMode = "none"
            }
        }
    }
}
