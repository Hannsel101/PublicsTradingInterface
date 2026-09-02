pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root

    implicitWidth: 720
    implicitHeight: 520

    readonly property bool compactLayout: width < 640
    readonly property color backgroundColor: "#08090a"
    readonly property color surfaceColor: "#101115"
    readonly property color elevatedColor: "#202126"
    readonly property color primaryText: "#f7f8f8"
    readonly property color secondaryText: "#d0d6e0"
    readonly property color mutedText: "#8a8f98"
    readonly property color borderColor: "#2f3138"
    readonly property color successColor: "#10b981"
    readonly property color pendingColor: "#7170ff"
    readonly property color dangerColor: "#ff5c7a"

    Rectangle {
        anchors.fill: parent
        radius: 24
        color: root.surfaceColor
        border.color: root.borderColor
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
                        text: ApiWorker.currentTransactionTitle
                        color: root.primaryText
                        font.pixelSize: root.compactLayout ? 24 : 32
                        font.weight: Font.DemiBold
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        text: ApiWorker.tradeResultsBusy
                              ? qsTr("Waiting for each loaded account to return a result…")
                              : qsTr("Every loaded account has returned a transaction result.")
                        color: ApiWorker.tradeResultsBusy ? root.mutedText : root.successColor
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
                model: ApiWorker.tradeResults

                delegate: Rectangle {
                    id: resultDelegate

                    required property var modelData

                    width: resultList.width
                    height: Math.max(96, resultLayout.implicitHeight + 28)
                    radius: 18
                    color: resultDelegate.status === "success"
                           ? "#0f2f24"
                           : resultDelegate.status === "failed"
                             ? "#3a1720"
                             : "#1f2140"
                    border.color: resultDelegate.status === "success"
                                  ? root.successColor
                                  : resultDelegate.status === "failed"
                                    ? root.dangerColor
                                    : root.pendingColor
                    border.width: 1

                    readonly property string accountId: resultDelegate.modelData.accountId || qsTr("Unknown account")
                    readonly property string accountLabel: resultDelegate.modelData.accountLabel || qsTr("Account %1").arg(resultDelegate.accountId)
                    readonly property string status: resultDelegate.modelData.status || "pending"
                    readonly property string message: resultDelegate.modelData.message || ""
                    readonly property bool success: resultDelegate.status === "success"

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
                                   ? root.successColor
                                   : resultDelegate.status === "failed"
                                     ? root.dangerColor
                                     : root.pendingColor

                            Label {
                                anchors.centerIn: parent
                                text: resultDelegate.status === "success" ? "✓" : resultDelegate.status === "failed" ? "!" : "…"
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
                                color: root.primaryText
                                font.pixelSize: 16
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }

                            Label {
                                Layout.fillWidth: true
                                text: resultDelegate.status === "success"
                                      ? qsTr("Trade success")
                                      : resultDelegate.status === "failed"
                                        ? qsTr("Trade failed")
                                        : qsTr("Pending")
                                color: resultDelegate.status === "success"
                                       ? root.successColor
                                       : resultDelegate.status === "failed"
                                         ? root.dangerColor
                                         : root.pendingColor
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }

                            Label {
                                Layout.fillWidth: true
                                text: resultDelegate.message
                                color: root.secondaryText
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                                maximumLineCount: 3
                                elide: Text.ElideRight
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
                enabled: ApiWorker.tradeResultsComplete
                text: ApiWorker.tradeResultsBusy ? qsTr("Waiting for accounts…") : qsTr("Done")

                contentItem: Text {
                    text: doneButton.text
                    color: doneButton.enabled ? "white" : root.mutedText
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: 15
                    color: doneButton.enabled ? root.pendingColor : root.elevatedColor
                    opacity: doneButton.enabled ? 1 : 0.7
                }

                onClicked: ApiWorker.dismissTradeResults()
            }
        }
    }
}
