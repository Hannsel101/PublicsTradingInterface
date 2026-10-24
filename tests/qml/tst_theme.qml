import QtQuick
import QtTest
import PublicsTradingInterface

TestCase {
    name: "Theme"

    function test_sharedPalette() {
        compare(String(Theme.backgroundColor), "#08090a")
        compare(String(Theme.surfaceColor), "#101115")
        compare(String(Theme.primaryText), "#f7f8f8")
        compare(String(Theme.accentColor), "#7170ff")
        compare(String(Theme.successColor), "#10b981")
        compare(String(Theme.dangerColor), "#ff5c7a")
        verify(Theme.surfaceColor !== Theme.cardColor)
    }
}
