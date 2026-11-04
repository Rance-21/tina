import QtQuick
import QtQuick.Effects

Item {
    id: root

    // Required reference to the full-window background source item for real-time frosted blur
    required property Item blurSource
    property int cornerRadius: 16
    // When true, the left edge is completely flat (0 radius on top-left and bottom-left) so it sits flush against the screen left edge
    property bool flatLeftEdge: false
    property color tintColor: "#78101624"
    property color borderColor: "#3894b8ff"
    property real borderWidth: 1.0
    property int blurRadius: 48
    property bool highlightGlow: false

    // Clip the blurred background into rounded corners (with optional flat left edge)
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
            live: true
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

        // Fill left corners to make left edge straight when flatLeftEdge is true
        Rectangle {
            visible: root.flatLeftEdge
            width: root.cornerRadius
            height: parent.height
            anchors.left: parent.left
            color: "#ffffff"
        }
    }

    // Outer glass rim
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

