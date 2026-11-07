pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects

Item {
    id: root

    // 采样背景和代码场景，生成真实毛玻璃效果。
    required property Item blurSource
    property point sampleOrigin: mapToItem(blurSource, 0, 0)
    property real cornerRadius: 16
    // 左侧标签贴齐屏幕边缘，不保留左上和左下圆角。
    property bool flatLeftEdge: false
    property color tintColor: "#78101624"
    property color borderColor: "#3894b8ff"
    property real borderWidth: 1.0
    property int blurRadius: 48
    property bool highlightGlow: false

    // 只截取面板下方的场景，半分辨率取样能减少纹理带宽。
    ShaderEffectSource {
        id: effectSource
        anchors.fill: parent
        visible: false // 原始纹理只供模糊采样。
        sourceItem: root.visible ? root.blurSource : null
        sourceRect: Qt.rect(root.sampleOrigin.x, root.sampleOrigin.y, root.width, root.height)
        textureSize: Qt.size(Math.max(1, Math.ceil(width / 2)), Math.max(1, Math.ceil(height / 2)))
    }

    // 在同一效果中完成模糊和圆角裁剪，省去一层离屏合成。
    MultiEffect {
        anchors.fill: parent
        source: effectSource
        blurEnabled: true
        blurMax: root.blurRadius
        autoPaddingEnabled: false
        blur: 1.0
        saturation: 0.15
        maskEnabled: true
        maskSource: maskItem
        maskThresholdMin: 0.5
        maskSpreadAtMin: 0.2
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

    // 色层和描边共用同一个圆角形状。
    Item {
        anchors.fill: parent
        clip: true

        Rectangle {
            x: root.flatLeftEdge ? -root.cornerRadius : 0
            y: 0
            width: parent.width + (root.flatLeftEdge ? root.cornerRadius : 0)
            height: parent.height
            radius: root.cornerRadius
            color: root.tintColor
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

