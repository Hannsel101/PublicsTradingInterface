# The network boundary is independent of the future applet UI and of Publics.
qt_add_library(rh_network_engine STATIC
    rhnetworkengine.cpp
    rhnetworkengine.h
)
target_include_directories(rh_network_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rh_network_engine PUBLIC Qt6::Network)

qt_add_library(rh_sessions STATIC
    rhcredentialstore.cpp rhcredentialstore.h
    rhsessionmanager.cpp rhsessionmanager_auth.cpp rhsessionmanager.h
)
target_include_directories(rh_sessions PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${QTKEYCHAIN_INCLUDE_DIRS}
    ${qtkeychain_SOURCE_DIR}
    ${qtkeychain_BINARY_DIR}
)
target_link_libraries(rh_sessions PUBLIC rh_network_engine ${PUBLICS_QTKEYCHAIN_TARGET})

qt_add_library(robin_hood_applet STATIC robinhoodapplet.cpp robinhoodapplet.h)
target_include_directories(robin_hood_applet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(robin_hood_applet PUBLIC applet_contract PRIVATE rh_sessions Qt6::Qml)
qt_add_resources(robin_hood_applet rh_config PREFIX "/robinhood" FILES config/robinhood-api.json)
add_subdirectory(qml/RobinHoodInterface)
