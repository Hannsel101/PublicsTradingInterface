qt_add_library(rh_ui STATIC)
qt_add_qml_module(rh_ui
    URI RobinHoodInterface
    CLASS_NAME RobinHoodInterfacePlugin
    QML_FILES
        CoreView.qml
        NativeLoginForm.qml
        AccountCard.qml
        AccountInfoDialog.qml
        StockTrade.qml
        TradeButton.qml
        TradeResultScreen.qml
        TradeResultCard.qml
        ResultText.qml
    RESOURCES
        assets/account-info.svg
)
target_link_libraries(rh_ui PRIVATE Qt6::Quick)
