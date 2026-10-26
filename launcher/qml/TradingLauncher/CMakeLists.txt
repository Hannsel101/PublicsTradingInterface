qt_add_library(launcher_ui STATIC)
qt_add_qml_module(launcher_ui
    URI TradingLauncher
    CLASS_NAME TradingLauncherPlugin
    QML_FILES LauncherWindow.qml AppletChoice.qml
    DEPENDENCIES TARGET publics_ui
)
target_link_libraries(launcher_ui PRIVATE Qt6::Quick)
