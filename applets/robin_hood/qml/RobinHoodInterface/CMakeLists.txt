qt_add_library(rh_ui STATIC)
qt_add_qml_module(rh_ui
    URI RobinHoodInterface
    CLASS_NAME RobinHoodInterfacePlugin
    QML_FILES CoreView.qml
)
target_link_libraries(rh_ui PRIVATE Qt6::Quick)
