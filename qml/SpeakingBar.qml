import QtQuick
import Halo

// The speaking bar: thirty-two glossy capsules driven by the real spectrum of
// whatever is audible right now — the microphone while listening, the
// synthesised voice while answering. When nothing is audible it keeps a slow,
// quiet breath so the instrument still feels alive.
Item {
    id: bars

    property real level: 0
    property string state: "idle"
    property bool reducedMotion: false

    readonly property real barWidth: 6
    readonly property real barGap: 8.4
    readonly property real maxHeight: 58
    readonly property int count: 32

    implicitHeight: 96
    implicitWidth: count * barWidth + (count - 1) * barGap

    // Ambient liveliness per phase: stillness reads as "not working", so each
    // state gets its own amount of breath.
    readonly property real ambient: {
        if (reducedMotion)
            return 0.0
        if (state === "listening") return 0.05
        if (state === "thinking") return 0.40
        if (state === "answering") return 0.30
        if (state === "speaking") return 0.03
        return 0.08
    }

    property real phase: 0
    NumberAnimation on phase {
        from: 0
        to: Math.PI * 2
        duration: 5200
        loops: Animation.Infinite
        running: !bars.reducedMotion
    }

    // Soft pool of light beneath the capsules.
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 60
        width: parent.width * 0.92
        height: 34
        radius: height / 2
        opacity: 0.5 + Math.min(bars.level, 0.6)
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#00000000" }
            GradientStop { position: 0.28; color: "#1A5EEAD4" }
            GradientStop { position: 0.5; color: "#385EEAD4" }
            GradientStop { position: 0.72; color: "#1A5EEAD4" }
            GradientStop { position: 1.0; color: "#00000000" }
        }
        Behavior on opacity { NumberAnimation { duration: 220 } }
    }

    // Hairline the capsules stand on.
    Rectangle {
        x: 0
        y: 72
        width: parent.width
        height: 1
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#00FFFFFF" }
            GradientStop { position: 0.5; color: "#26FFFFFF" }
            GradientStop { position: 1.0; color: "#00FFFFFF" }
        }
    }

    Item {
        id: capsules
        anchors.horizontalCenter: parent.horizontalCenter
        y: 0
        width: bars.count * bars.barWidth + (bars.count - 1) * bars.barGap
        height: 72
        scale: 1 + bars.level * 0.018
        transformOrigin: Item.Bottom
        Behavior on scale { NumberAnimation { duration: 160; easing.type: Easing.OutQuad } }

        Repeater {
            model: app.spectrum
            delegate: Item {
                id: capsule

                x: index * (bars.barWidth + bars.barGap)
                width: bars.barWidth
                height: capsules.height

                readonly property real wave: Math.max(0.0, 0.5 + 0.42 * Math.sin(bars.phase + index * 0.44)
                                                    + 0.22 * Math.sin(bars.phase * 2.1 - index * 0.21))
                readonly property real audio: Math.max(0, model.level)
                readonly property real peakLevel: Math.max(0, model.peak)
                readonly property real shape: Math.min(1, Math.pow(audio * 0.92 + bars.ambient * (0.2 + 0.8 * wave), 0.82))
                readonly property real h: 3 + shape * bars.maxHeight
                readonly property real peakH: 3 + Math.min(1, peakLevel * 0.92 + bars.ambient * 0.4) * bars.maxHeight

                // Track dot: keeps the bar legible at zero.
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 3
                    radius: 1.5
                    color: "#1AFFFFFF"
                }

                // The capsule itself: white-hot top, accent body.
                Rectangle {
                    id: body
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: capsule.h
                    radius: 3
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#F7FFFFFF" }
                        GradientStop { position: 0.22; color: "#EBDCFFF9" }
                        GradientStop { position: 0.55; color: "#E05EEAD4" }
                        GradientStop { position: 1.0; color: "#8C2C9185" }
                    }
                    Behavior on height { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }

                    // Gloss: the specular line that makes it read as glass.
                    Rectangle {
                        anchors.top: parent.top
                        anchors.topMargin: 2.5
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: Math.max(1, parent.width - 3)
                        height: 2
                        radius: 1
                        color: "#E6FFFFFF"
                        opacity: 0.35 + capsule.shape * 0.6
                    }
                }

                // Peak hold: a single bright hairline that trails the capsule.
                Rectangle {
                    width: parent.width
                    height: 1.5
                    radius: 1
                    color: "#B3E9FFFA"
                    opacity: capsule.peakH - capsule.h > 2 ? 0.75 : 0
                    y: parent.height - capsule.peakH
                    Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutQuad } }
                    Behavior on opacity { NumberAnimation { duration: 180 } }
                }
            }
        }
    }
}
