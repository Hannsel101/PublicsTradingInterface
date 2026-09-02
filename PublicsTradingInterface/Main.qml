import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import "qml/DisplayAccountInformation"
import "qml/StockSearchAutoComplete"
import "qml/PerformTrades"


ApplicationWindow {
    id: window
    width: 640
    height: 480
    minimumWidth: 200
    minimumHeight: 250
    visible: true
    title: qsTr("Publics Brokerage API Multi-Account Trader")
    property bool lightMode: Application.styleHints.colorScheme === Qt.Light
    property color reallyDark: "#1f1f1f"
    property color dark: "#262626"
    property color reallyLight: "#e7e7e7"
    property color light: "#e0e0e0"


    /**
      * When the
      */
    property bool tokenValid: false

    // Custom action for submitted secret keys. Never log key material.
    function printUserSecretKey(secretKey)
    {
        userInputField.clear()
    }

    ValidityTimer
    {
        id: testTimer
        anchors.top: parent.top
        anchors.left: parent.left
        width: 100
        height: 50
    }

    /**
      * a widget that performs the purchase and selling of stocks
      */
    StockTrade
    {
        id: stockTradeInterface
        anchors.centerIn: parent
        visible: StockSearchController.tokenActive

        onPerformBuy:
        {
            ApiWorker.executePreflight(inputFieldText, "BUY")
        }

        onPerformSell:
        {
            ApiWorker.executePreflight(inputFieldText, "SELL")
        }
    }

    /**
      * Api Key List displays labels for securely stored Public Brokerage
      * API keys in the platform keychain.
      */
    ApiKeyList
    {
        id: apiKeys
        visible: !StockSearchController.tokenActive
        anchors.horizontalCenter: window.horizontalCenter
        anchors.top: window.top
        anchors.topMargin: window.height*0.10
    }

    /**
      * Will be added in a future update to give ticker suggestions when the user
      * begins typing part of a possible ticker symbol
      */
    // StockSearchAutoComplete
    // {
    //     id: findTickerSymbol
    //     anchors.top: userKeyInput.bottom
    //     anchors.horizontalCenter: userKeyInput.horizontalCenter
    //     visible: false
    // }

    /**
      * TO DO: add functionality later so that the user can add new keys and it will store it in windows under
      *        some type of credential manager group. And be accessed for subsequent runs
      */
    Column
    {
        id: userKeyInput
        anchors.centerIn: parent
        spacing: 15
        visible: !AuthClient.sessionActive

        TextField
        {
            id: userInputField
            placeholderText: "Enter your secret key here..."
            width: 250

            // Triggered automatically when the user presses Enter/Return
            onAccepted:
            {
               printUserSecretKey(userInputField.text)
            }
        }

        Button
        {
            text: "Submit"

            // Triggered when clicking the button manually
            onClicked:
            {
                printUserSecretKey(userInputField.text)
            }
        }
        z:3
    }
}
