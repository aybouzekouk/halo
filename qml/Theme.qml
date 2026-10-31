pragma Singleton

import QtQuick

// One place for the whole visual language: type scale (16 px base, minor third),
// semantic colour roles, and the geometry of the glass.
QtObject {
    id: theme

    readonly property string sans: Qt.application.font.family.length > 0
                                   ? Qt.application.font.family : "sans-serif"

    // Modular scale: 16 * 1.2^n
    readonly property real micro: 12
    readonly property real caption: 13
    readonly property real body: 16
    readonly property real lead: 21
    readonly property real display: 25

    // Surfaces (AARRGGBB)
    readonly property color panelTop: "#F211161F"
    readonly property color panelBottom: "#F6070A0F"
    readonly property color rimTop: "#2EFFFFFF"
    readonly property color rimBottom: "#0AFFFFFF"
    readonly property color divider: "#14FFFFFF"

    // Text roles
    readonly property color textHi: "#F3F6F8"
    readonly property color textMid: "#B6C0CB"
    readonly property color textLo: "#8D97A3"

    // Signal colours
    readonly property color accent: "#5EEAD4"
    readonly property color accentDim: "#2F9C8C"
    readonly property color violet: "#A78BFA"
    readonly property color warm: "#FDE9C8"
    readonly property color danger: "#FCA5A5"

    readonly property int radiusPanel: 24
    readonly property int gutter: 30
    readonly property real motionFast: 140
    readonly property real motionBase: 240
    readonly property real motionSlow: 460
}
