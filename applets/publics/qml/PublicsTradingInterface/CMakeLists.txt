set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_library(publics_ui STATIC)
qt_add_qml_module(publics_ui
    URI PublicsTradingInterface
    CLASS_NAME PublicsTradingInterfacePlugin
    QML_FILES
        CoreView.qml
        Theme.qml
        credentials/ApiKeyList.qml
        credentials/SwitchKeyButton.qml
        credentials/SessionTimer.qml
        trading/StockTrade.qml
        trading/TradeResultScreen.qml
        trading/AutoTradeScreen.qml
    RESOURCES
        credentials/icons/edit.png
        credentials/icons/delete.png
        credentials/icons/edit.svg
        credentials/icons/delete.svg
)
target_link_libraries(publics_ui PRIVATE Qt6::Quick)
