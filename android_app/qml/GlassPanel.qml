pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects

Item {
    id: root

    // 采样背景和代码场景，生成真实毛玻璃效果。
    required property Item blurSource
    property int cornerRadius: 16
    // 左侧标签贴齐屏幕边缘，不保留左上和左下圆角。
    property bool flatLeftEdge: false
    property color tintColor: "#78101624"
    property color borderColor: "#3894b8ff"
    property real borderWidth: 1.0
    property int blurRadius: 48
    property bool highlightGlow: false

    // 圆角遮罩裁剪背景；隐藏的面板不需要持续更新纹理。
    Item {
        id: clipContainer
        anchors.fill: parent
        layer.enabled: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: maskItem
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }

        ShaderEffectSource {
            id: effectSource
            anchors.fill: parent
            sourceItem: root.blurSource
            sourceRect: root.blurSource
                        ? root.mapToItem(root.blurSource, 0, 0, root.width, root.height)
                        : Qt.rect(0, 0, root.width, root.height)
            live: root.visible
            // 平板上以半分辨率采样，模糊仍平滑，减少纹理带宽和离屏渲染开销。
            textureSize: Qt.size(Math.max(1, Math.ceil(width / 2)), Math.max(1, Math.ceil(height / 2)))
        }

        MultiEffect {
            anchors.fill: effectSource
            source: effectSource
            blurEnabled: true
            blurMax: root.blurRadius
            blur: 1.0
            saturation: 0.15
        }

        Rectangle {
            anchors.fill: parent
            color: root.tintColor
        }
    }

    Item {
        id: maskItem
        anchors.fill: parent
        visible: false
        layer.enabled: true

        Rectangle {
            anchors.fill: parent
            radius: root.cornerRadius
        }

        // 填平左侧圆角。
        Rectangle {
            visible: root.flatLeftEdge
            width: root.cornerRadius
            height: parent.height
            anchors.left: parent.left
            color: "#ffffff"
        }
    }

    // 毛玻璃外边缘。
    Item {
        anchors.fill: parent
        clip: true

        Rectangle {
            x: root.flatLeftEdge ? -root.cornerRadius : 0
            y: 0
            width: parent.width + (root.flatLeftEdge ? root.cornerRadius : 0)
            height: parent.height
            radius: root.cornerRadius
            color: "transparent"
            border.color: root.borderColor
            border.width: root.borderWidth

            Rectangle {
                anchors.fill: parent
                anchors.margins: 1
                radius: Math.max(0, root.cornerRadius - 1)
                color: "transparent"
                border.color: root.highlightGlow ? "#457cc4ff" : "#14ffffff"
                border.width: 1
            }
        }
    }
}

