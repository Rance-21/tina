import QtQuick

// 使用路径绘制，避免 Android 字体缺少符号时出现方框；颜色、方向和尺寸变化都会重绘。
Canvas {
    id: icon
    required property string name
    property color color: "#d0e0ff"
    implicitWidth: 20
    implicitHeight: 20
    onNameChanged: requestPaint()
    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onVisibleChanged: if (visible) requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.clearRect(0, 0, width, height)
        ctx.scale(width / 24, height / 24)
        ctx.strokeStyle = icon.color
        ctx.fillStyle = icon.color
        ctx.lineWidth = 1.8
        ctx.lineCap = "round"
        ctx.lineJoin = "round"
        ctx.beginPath()
        switch (icon.name) {
        case "terminal":
            ctx.moveTo(4, 6)
            ctx.lineTo(10, 12)
            ctx.lineTo(4, 18)
            ctx.moveTo(13, 18)
            ctx.lineTo(21, 18)
            break
        case "folder":
            ctx.moveTo(2, 5)
            ctx.lineTo(9, 5)
            ctx.lineTo(12, 8)
            ctx.lineTo(22, 8)
            ctx.lineTo(22, 20)
            ctx.lineTo(2, 20)
            ctx.closePath()
            break
        case "file":
            ctx.moveTo(5, 2)
            ctx.lineTo(14, 2)
            ctx.lineTo(19, 7)
            ctx.lineTo(19, 22)
            ctx.lineTo(5, 22)
            ctx.closePath()
            ctx.moveTo(14, 2)
            ctx.lineTo(14, 7)
            ctx.lineTo(19, 7)
            ctx.moveTo(8, 12)
            ctx.lineTo(16, 12)
            ctx.moveTo(8, 16)
            ctx.lineTo(16, 16)
            break
        case "chevron-down":
            ctx.moveTo(6, 9)
            ctx.lineTo(12, 15)
            ctx.lineTo(18, 9)
            break
        case "chevron-right":
            ctx.moveTo(9, 6)
            ctx.lineTo(15, 12)
            ctx.lineTo(9, 18)
            break
        case "arrow-up":
        case "link":
            if (icon.name === "link") {
                ctx.moveTo(6, 18)
                ctx.lineTo(18, 6)
                ctx.moveTo(7, 6)
                ctx.lineTo(18, 6)
                ctx.lineTo(18, 17)
            } else {
                ctx.moveTo(12, 20)
                ctx.lineTo(12, 4)
                ctx.moveTo(5, 11)
                ctx.lineTo(12, 4)
                ctx.lineTo(19, 11)
            }
            break
        case "branch":
            ctx.moveTo(6, 7)
            ctx.lineTo(6, 17)
            ctx.moveTo(18, 7)
            ctx.bezierCurveTo(18, 13, 6, 9, 6, 15)
            ctx.stroke()
            ctx.beginPath()
            for (const point of [[6, 4], [6, 20], [18, 4]]) {
                ctx.moveTo(point[0] + 2.5, point[1])
                ctx.arc(point[0], point[1], 2.5, 0, Math.PI * 2)
            }
            break
        case "more":
            for (const x of [5, 12, 19]) {
                ctx.moveTo(x + 1.5, 12)
                ctx.arc(x, 12, 1.5, 0, Math.PI * 2)
            }
            ctx.fill()
            return
        }
        ctx.stroke()
    }
}
