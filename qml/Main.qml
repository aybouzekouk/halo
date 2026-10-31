import QtQuick
import Halo
import QtQuick.Window

Window {
    id: root

    width: 960
    height: 640
    visible: false
    color: "transparent"
    title: qsTr("Halo")
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool | Qt.NoDropShadowWindowHint

    readonly property color stateColor: {
        switch (app.state) {
        case "listening": return Theme.accent
        case "thinking":  return Theme.violet
        case "answering": return Theme.warm
        case "speaking":  return Theme.accent
        case "error":     return Theme.danger
        default:          return Theme.textLo
        }
    }

    Shortcut { sequence: "Escape"; onActivated: app.dismiss() }
    Shortcut { sequence: "Ctrl+Space"; onActivated: app.toggle() }

    Item {
        id: stage
        anchors.fill: parent
        focus: true
        opacity: app.shown ? 1 : 0

        // Typing is an alternative to speaking: the first printable key simply
        // starts the footer field, so there is no caret sitting in an empty box.
        Keys.onPressed: (event) => {
            if (!askInput.activeFocus && event.text.length > 0
                    && !(event.modifiers & Qt.ControlModifier)
                    && !(event.modifiers & Qt.AltModifier)) {
                askInput.forceActiveFocus()
                askInput.insert(event.text)
                event.accepted = true
            } else if (event.key === Qt.Key_Escape) {
                app.dismiss()
                event.accepted = true
            }
        }
        Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

        // ------------------------------------------------------------- bloom
        Item {
            id: bloomGroup
            anchors.centerIn: parent
            width: parent.width
            height: parent.height
            opacity: app.shown ? 1 : 0
            scale: (app.shown ? 1 : 0.9) * (1 + app.level * 0.03)
            Behavior on opacity { NumberAnimation { duration: 460; easing.type: Easing.OutCubic } }
            Behavior on scale { NumberAnimation { duration: 680; easing.type: Easing.OutCubic } }

            Image {
                anchors.fill: parent
                source: "image://bloom/glow"
                fillMode: Image.Stretch
                smooth: true
                opacity: 0.9
            }
        }

        // ------------------------------------------------------------- panel
        Item {
            id: panel
            anchors.centerIn: parent
            width: 660
            height: 440

            transform: Translate {
                id: panelShift
                y: app.shown ? 0 : 14
                Behavior on y { NumberAnimation { duration: 360; easing.type: Easing.OutCubic } }
            }

            opacity: app.shown ? 1 : 0
            scale: app.shown ? 1 : 0.985
            Behavior on opacity { NumberAnimation { duration: Theme.motionBase; easing.type: Easing.OutCubic } }
            Behavior on scale { NumberAnimation { duration: 340; easing.type: Easing.OutCubic } }

            // Ground shadow so the glass separates from its own glow.
            Image {
                anchors.centerIn: parent
                width: parent.width * 1.12
                height: parent.height * 1.35
                source: "image://bloom/shadow"
                fillMode: Image.Stretch
                smooth: true
                opacity: 0.55
                z: -1
            }

            // Rim light: a hairline gradient frame around the glass.
            Rectangle {
                anchors.fill: parent
                radius: Theme.radiusPanel + 1
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Theme.rimTop }
                    GradientStop { position: 0.45; color: "#12FFFFFF" }
                    GradientStop { position: 1.0; color: Theme.rimBottom }
                }
            }

            // Glass body.
            Rectangle {
                anchors.fill: parent
                anchors.margins: 1
                radius: Theme.radiusPanel
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Theme.panelTop }
                    GradientStop { position: 0.62; color: "#EF0B0E15" }
                    GradientStop { position: 1.0; color: Theme.panelBottom }
                }
            }

            // Top sheen: light landing on the upper edge.
            Rectangle {
                anchors.top: parent.top
                anchors.topMargin: 1
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 1
                anchors.rightMargin: 1
                height: 120
                radius: Theme.radiusPanel
                opacity: 0.5
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#10FFFFFF" }
                    GradientStop { position: 1.0; color: "#00FFFFFF" }
                }
            }

            // --------------------------------------------------------- content
            Item {
                anchors.fill: parent
                anchors.margins: Theme.gutter

                // header
                Row {
                    id: brand
                    anchors.left: parent.left
                    anchors.top: parent.top
                    spacing: 9
                    height: 14

                    Item {
                        width: 14
                        height: 14
                        anchors.verticalCenter: parent.verticalCenter

                        Rectangle {
                            anchors.centerIn: parent
                            width: 14
                            height: 14
                            radius: 7
                            color: root.stateColor
                            opacity: 0.16 + Math.min(app.level, 1) * 0.30
                            scale: 1 + Math.min(app.level, 1) * 0.5
                            Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: 6
                            height: 6
                            radius: 3
                            color: root.stateColor
                            Behavior on color { ColorAnimation { duration: 220 } }
                        }
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "HALO"
                        color: Theme.textLo
                        font.family: Theme.sans
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        font.letterSpacing: 3.4
                    }
                }

                Text {
                    id: phase
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: 1
                    text: app.phaseLabel.toUpperCase()
                    color: root.stateColor
                    opacity: app.state === "idle" || app.state === "hidden" ? 0.55 : 0.9
                    font.family: Theme.sans
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    font.letterSpacing: 2.2
                    Behavior on color { ColorAnimation { duration: 220 } }
                }

                // what was asked
                Text {
                    id: question
                    y: 30
                    width: parent.width
                    height: 54
                    text: app.question.length > 0 ? app.question : root.placeholderText
                    color: app.question.length > 0 ? Theme.textHi : Theme.textLo
                    font.family: Theme.sans
                    font.pixelSize: Theme.lead
                    font.weight: Font.Medium
                    lineHeight: 1.32
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignTop
                    Behavior on color { ColorAnimation { duration: 220 } }
                }

                SpeakingBar {
                    id: bars
                    x: 0
                    y: 96
                    width: parent.width
                    height: 96
                    level: app.level
                    state: app.state
                    reducedMotion: app.reducedMotion
                }

                // the answer
                Item {
                    id: answerArea
                    y: 200
                    width: parent.width
                    height: 126
                    clip: true

                    Flickable {
                        id: answerScroll
                        anchors.fill: parent
                        contentWidth: width
                        contentHeight: answerText.height
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.VerticalFlick
                        interactive: contentHeight > height
                        onContentHeightChanged: contentY = 0

                        Text {
                            id: answerText
                            width: answerScroll.width
                            text: app.answer
                            color: Theme.textMid
                            visible: text.length > 0
                            font.family: Theme.sans
                            font.pixelSize: Theme.body
                            lineHeight: 1.55
                            wrapMode: Text.Wrap
                        }
                    }

                    // While the answer is being produced, hold its shape: a
                    // skeleton reads as "writing" where a spinner reads as "stuck".
                    Column {
                        y: 8
                        spacing: 13
                        width: parent.width
                        visible: app.answer.length === 0 && app.state === "thinking"

                        Repeater {
                            model: [1.0, 0.74]
                            Rectangle {
                                width: parent.width * modelData
                                height: 11
                                radius: 5.5
                                color: "#12FFFFFF"
                                opacity: 0.3
                                SequentialAnimation on opacity {
                                    loops: Animation.Infinite
                                    PauseAnimation { duration: index * 150 }
                                    NumberAnimation { from: 0.28; to: 0.7; duration: 760; easing.type: Easing.InOutQuad }
                                    NumberAnimation { from: 0.7; to: 0.28; duration: 760; easing.type: Easing.InOutQuad }
                                    PauseAnimation { duration: (1 - index) * 150 }
                                }
                            }
                        }
                    }

                    Text {
                        visible: app.state === "error" && app.errorText.length > 0
                        width: parent.width
                        y: 4
                        text: app.errorText
                        color: Theme.danger
                        opacity: 0.9
                        font.family: Theme.sans
                        font.pixelSize: Theme.caption
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 24
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#00000000" }
                            GradientStop { position: 1.0; color: "#F0080B10" }
                        }
                    }
                }

                // footer
                Rectangle {
                    y: 338
                    width: parent.width
                    height: 1
                    color: Theme.divider
                }

                TextInput {
                    id: askInput
                    y: 346
                    width: parent.width - 215
                    height: 22
                    color: Theme.textMid
                    selectionColor: Theme.accent
                    selectedTextColor: "#080A0E"
                    font.family: Theme.sans
                    font.pixelSize: Theme.caption
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    onAccepted: {
                        if (text.trim().length > 0)
                            app.submitText(text)
                        text = ""
                    }
                    Keys.onEscapePressed: app.dismiss()

                    Text {
                        anchors.fill: parent
                        verticalAlignment: Text.AlignVCenter
                        visible: askInput.text.length === 0
                        text: qsTr("Type a question…")
                        color: Theme.textLo
                        opacity: 0.75
                        font.family: Theme.sans
                        font.pixelSize: Theme.caption
                    }
                }

                Text {
                    id: status
                    anchors.right: parent.right
                    y: 346
                    text: app.statusText
                    color: app.demo ? Theme.warm : Theme.textLo
                    opacity: app.demo ? 0.8 : 0.55
                    font.family: Theme.sans
                    font.pixelSize: 11
                    font.letterSpacing: 0.4
                }
            }
        }

        // hint below the glass
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: panel.bottom
            anchors.topMargin: 20
            text: app.hintText
            color: Theme.textLo
            opacity: app.shown ? 0.6 : 0
            font.family: Theme.sans
            font.pixelSize: Theme.micro
            font.letterSpacing: 0.5
            Behavior on opacity { NumberAnimation { duration: 260 } }
        }
    }

    readonly property string placeholderText: {
        switch (app.state) {
        case "listening": return qsTr("I'm listening…")
        case "thinking":  return qsTr("One moment…")
        case "speaking":  return app.question.length > 0 ? qsTr("…") : qsTr("What would you like to know?")
        case "error":     return qsTr("Something went wrong.")
        default:          return qsTr("What would you like to know?")
        }
    }

    onActiveChanged: {
        if (active)
            stage.forceActiveFocus()
    }
}
