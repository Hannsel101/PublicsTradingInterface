qt_add_library(telegram_bots_ui STATIC)
set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(telegram_bots_ui
    URI TelegramBotsInterface
    CLASS_NAME TelegramBotsInterfacePlugin
    QML_FILES Theme.qml CoreView.qml BotManagementPanel.qml BotActionButton.qml
        BotEditor.qml BotCredentialField.qml
        BotListRow.qml BotActivityBadge.qml
        BotExitConfirmation.qml BotSecurityDialog.qml BotInspectorConflictDialog.qml
    RESOURCES assets/robot-head.svg
)
target_link_libraries(telegram_bots_ui PRIVATE Qt6::Quick)
