import QtQuick
import QtQuick.Controls.Basic

Button {
    id: choice
    required property color stripeColor
    property string appletName: ""
    property url artworkSource
    readonly property bool hasArtwork: artworkSource.toString().length > 0
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                           hasArtwork ? choiceMetrics.advanceWidth + 172
                                      : choiceMetrics.advanceWidth + leftPadding + rightPadding)
    text: appletName
    font.pixelSize: 18
    font.bold: true
    height: 76
    implicitHeight: 76
    Accessible.name: appletName + (enabled ? "" : qsTr(" (coming soon)"))

    TextMetrics {
        id: choiceMetrics
        text: choice.text
        font: choice.font
    }

    background: Rectangle {
        radius: 10
        clip: true
        color: !choice.enabled ? "#18191d" : choice.pressed ? "#292b31" : "#202126"
        border.color: !choice.enabled ? "#35383f" : choice.activeFocus ? "#d0d6e0" : "#42444d"
        border.width: choice.activeFocus ? 2 : 1
        Rectangle {
            id: stripe
            objectName: "appletStripe"
            width: 16
            height: parent.height
            x: choice.hasArtwork
               ? Math.max(parent.width * 0.7 - width / 2,
                          (parent.width + choiceMetrics.advanceWidth) / 2 + 12)
               : parent.width * 0.7 - width / 2
            color: choice.stripeColor
            opacity: choice.enabled ? 1 : 0.65
        }
        Item {
            anchors.left: stripe.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            visible: choice.hasArtwork
            Image {
                objectName: "appletArtwork"
                anchors.centerIn: parent
                width: Math.max(0, Math.min(44, parent.width - 16, parent.height - 16))
                height: width
                source: choice.artworkSource
                sourceSize.width: 96
                sourceSize.height: 96
                fillMode: Image.PreserveAspectFit
                Accessible.ignored: true
            }
        }
    }
    contentItem: Text {
        text: choice.text
        color: choice.enabled ? "#f7f8f8" : "#9299a3"
        font: choice.font
        horizontalAlignment: Text.AlignHCenter

        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
