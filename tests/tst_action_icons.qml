import QtQuick
import QtTest

TestCase {
    name: "ActionIcons"
    when: windowShown

    Image {
        id: editImage
        source: "../qml/DisplayAccountInformation/icons/edit.png"
    }
    Image {
        id: deleteImage
        source: "../qml/DisplayAccountInformation/icons/delete.png"
    }

    function test_iconImagesDecode() {
        tryCompare(editImage, "status", Image.Ready)
        tryCompare(deleteImage, "status", Image.Ready)
        compare(editImage.implicitWidth, 72)
        compare(deleteImage.implicitWidth, 72)
    }
}
