import QtQuick
import QtTest

TestCase {
    name: "ActionIcons"
    when: windowShown

    Image {
        id: editImage
        source: "qrc:/qt/qml/PublicsTradingInterface/credentials/icons/edit.png"
    }
    Image {
        id: deleteImage
        source: "qrc:/qt/qml/PublicsTradingInterface/credentials/icons/delete.png"
    }

    function test_iconImagesDecode() {
        tryCompare(editImage, "status", Image.Ready)
        tryCompare(deleteImage, "status", Image.Ready)
        compare(editImage.implicitWidth, 72)
        compare(deleteImage.implicitWidth, 72)
    }
}
