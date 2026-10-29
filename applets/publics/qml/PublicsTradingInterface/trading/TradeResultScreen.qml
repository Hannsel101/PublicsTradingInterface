pragma ComponentBehavior: Bound

import QtQuick
import PublicsTradingInterface
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root

    property var resultController: ApiWorker

    implicitWidth: 720
    implicitHeight: 520

    readonly property bool compactLayout: width < 640

    Rectangle {
        anchors.fill: parent
        radius: 24
        color: Theme.surfaceColor
        border.color: Theme.borderColor
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: root.compactLayout ? 16 : 22
            spacing: root.compactLayout ? 14 : 18

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        Layout.fillWidth: true
                        text: root.resultController.currentTransactionTitle
                        color: Theme.primaryText
                        font.pixelSize: root.compactLayout ? 24 : 32
                        font.weight: Font.DemiBold
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.resultController.tradeResultsBusy
                              ? qsTr("Waiting for each account and ticker to return a result…")
                              : qsTr("Processing complete. Review each outcome before submitting again.")
                        color: Theme.mutedText
                        font.pixelSize: 14
                        wrapMode: Text.WordWrap
                    }
                }
            }

            ListView {
                id: resultList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 12
                model: root.resultController.tradeResults

                delegate: Rectangle {
                    id: resultDelegate

                    required property var modelData

                    width: resultList.width
                    height: Math.max(96, resultLayout.implicitHeight + 28)
                    radius: 18
                    color: resultDelegate.status === "success"
                           ? Theme.successSurface
                           : resultDelegate.status === "failed"
                             ? Theme.dangerSurface
                             : Theme.pendingSurface
                    border.color: resultDelegate.status === "success"
                                  ? Theme.successColor
                                  : resultDelegate.status === "failed"
                                    ? Theme.dangerColor
                                    : Theme.accentColor
                    border.width: 1

                    readonly property string accountId: resultDelegate.modelData.accountId || qsTr("Unknown account")
                    readonly property string accountLabel: (resultDelegate.modelData.accountLabel || qsTr("Account %1").arg(resultDelegate.accountId))
                                                           + (resultDelegate.modelData.symbol ? " · " + resultDelegate.modelData.symbol : "")
                    readonly property string status: resultDelegate.modelData.status || "pending"
                    readonly property string message: resultDelegate.modelData.message || ""


                    RowLayout {
                        id: resultLayout
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 12

                        Rectangle {
                            Layout.preferredWidth: 44
                            Layout.preferredHeight: 44
                            radius: 22
                            color: resultDelegate.status === "success"
                                   ? Theme.successColor
                                   : resultDelegate.status === "failed"
                                     ? Theme.dangerColor
                                     : Theme.accentColor

                            Label {
                                anchors.centerIn: parent
                                text: resultDelegate.status === "success" ? "✓" : resultDelegate.status === "pending" ? "…" : "!"
                                color: "white"
                                font.pixelSize: 22
                                font.weight: Font.Bold
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 5

                            Label {
                                Layout.fillWidth: true
                                text: resultDelegate.accountLabel
                                color: Theme.primaryText
                                font.pixelSize: 16
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }

                            Label {
                                Layout.fillWidth: true
                                text: resultDelegate.status === "success"
                                      ? qsTr("Request acknowledged")
                                      : resultDelegate.status === "failed"
                                        ? qsTr("Trade failed")
                                        : resultDelegate.status === "uncertain"
                                          ? qsTr("Outcome uncertain")
                                          : resultDelegate.status === "not_submitted"
                                            ? qsTr("Not submitted")
                                            : qsTr("Pending")
                                color: resultDelegate.status === "success"
                                       ? Theme.successColor
                                       : resultDelegate.status === "failed"
                                         ? Theme.dangerColor
                                         : Theme.accentColor
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }

                            Label {
                                Layout.fillWidth: true
                                text: resultDelegate.message
                                color: Theme.secondaryText
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                                textFormat: Text.PlainText
                            }
                        }
                    }
                }

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }
            }

            Button {
                id: doneButton
                Layout.fillWidth: true
                Layout.minimumHeight: 52
                enabled: root.resultController.tradeResultsComplete
                text: root.resultController.tradeResultsBusy ? qsTr("Waiting for accounts…") : qsTr("Done")

                contentItem: Text {
                    text: doneButton.text
                    color: doneButton.enabled ? "white" : Theme.mutedText
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: 15
                    color: doneButton.enabled ? Theme.accentColor : Theme.elevatedColor
                    opacity: doneButton.enabled ? 1 : 0.7
                }

                onClicked: root.resultController.dismissTradeResults()
            }
        }
    }
}
