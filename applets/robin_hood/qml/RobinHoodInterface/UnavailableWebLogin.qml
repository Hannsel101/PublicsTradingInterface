import QtQuick
import QtQuick.Controls

Label {
    text: qsTr("Secure browser sign-in is unavailable. Desktop requires QtWebEngineQuick. Android QtWebView sign-in is disabled until native pre-navigation enforcement is implemented.")
    color: "#abc4b8"
    wrapMode: Text.Wrap
    verticalAlignment: Text.AlignVCenter
    horizontalAlignment: Text.AlignHCenter
    padding: 12
}
