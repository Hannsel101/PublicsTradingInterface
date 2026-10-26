qt_add_library(rh_ui STATIC)
qt_add_qml_module(rh_ui
    URI RobinHoodInterface
    CLASS_NAME RobinHoodInterfacePlugin
    QML_FILES
        CoreView.qml
        WebLoginPanel.qml
        UnavailableWebLogin.qml
)
target_link_libraries(rh_ui PRIVATE Qt6::Quick)
if(PUBLICS_RH_BROWSER_BACKEND STREQUAL "webengine")
    qt_target_qml_sources(rh_ui QML_FILES DesktopWebLogin.qml)
    target_link_libraries(rh_ui PRIVATE Qt6::WebEngineQuick)
elseif(PUBLICS_RH_BROWSER_BACKEND STREQUAL "android")
    qt_target_qml_sources(rh_ui QML_FILES AndroidWebLogin.qml)
    target_link_libraries(rh_ui PRIVATE Qt6::WebView)
endif()
