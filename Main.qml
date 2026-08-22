import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import QtQuick.Window
import "qml/DisplayAccountInformation"
import "qml/StockSearchAutoComplete"
import "qml/PerformTrades"


ApplicationWindow {
    id: window
    width: 1000
    height: 700
    visible: true
    title: qsTr("Publics Brokerage API Multi-Account Trader")
    property bool lightMode: Application.styleHints.colorScheme === Qt.Light
    property color reallyDark: "#1f1f1f"
    property color dark: "#262626"
    property color reallyLight: "#e7e7e7"
    property color light: "#e0e0e0"
    color: "#8E9294"


    /**
      * When the
      */
    property bool tokenValid: false

    // Custom Action Function
    function printUserSecretKey(secretKey)
    {
        console.log("User submitted key:", secretKey)
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

            if(isDebugMode)
            {
                ApiWorker.executePreflight(inputFieldText, "BUY")
            }
            else
            {
                ApiWorker.executeTrade(inputFieldText , "BUY")
            }
        }

        onPerformSell:
        {
            ApiWorker.executePreflight(inputFieldText, "SELL")
        }
    }

    /**
      * Api Key List displays a list of securely stored Public Brokerage
      * Api Keys that are stored in Windows Credential Manager
      */
    ApiKeyList
    {
        id: apiKeys
        visible: !StockSearchController.tokenActive
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: parent.height*0.10
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
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: apiKeys.bottom
        anchors.topMargin: 10
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
            text: "Store New Key"

            // Triggered when clicking the button manually
            onClicked:
            {
                printUserSecretKey(userInputField.text)
            }
        }
        z:3
    }

    Component.onCompleted:
    {

    }
}
