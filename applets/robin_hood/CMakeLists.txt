# The network boundary is independent of the future applet UI and of Publics.
qt_add_library(rh_network_engine STATIC
    rhnetworkengine.cpp
    rhnetworkengine.h
)
target_include_directories(rh_network_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rh_network_engine PUBLIC Qt6::Network)

# Browser support belongs only to Robinhood; other applets retain their dependencies.
set(PUBLICS_RH_BROWSER_BACKEND "unavailable")
if(ANDROID AND TARGET Qt6::WebView)
    set(PUBLICS_RH_BROWSER_BACKEND "android")
elseif(NOT ANDROID AND NOT IOS AND TARGET Qt6::WebEngineQuick)
    set(PUBLICS_RH_BROWSER_BACKEND "webengine")
endif()

qt_add_library(rh_browser_auth STATIC rhbrowserlogin.cpp rhbrowserlogin.h)
set_property(TARGET rh_browser_auth PROPERTY PUBLICS_RH_BROWSER_BACKEND "${PUBLICS_RH_BROWSER_BACKEND}")
target_include_directories(rh_browser_auth PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rh_browser_auth PUBLIC Qt6::Network Qt6::Qml)
if(PUBLICS_RH_BROWSER_BACKEND STREQUAL "webengine")
    target_compile_definitions(rh_browser_auth PRIVATE PUBLICS_RH_WEBENGINE=1)
    target_link_libraries(rh_browser_auth PRIVATE Qt6::WebEngineQuick)
elseif(PUBLICS_RH_BROWSER_BACKEND STREQUAL "android")
    # Module availability is not a security gate; production login stays disabled.
    message(STATUS "Robinhood Android login disabled pending a verified native pre-navigation interceptor.")
    target_compile_definitions(rh_browser_auth PRIVATE PUBLICS_RH_ANDROID_WEBVIEW=1)
    target_link_libraries(rh_browser_auth PRIVATE Qt6::WebView)
else()
    message(STATUS "Robinhood web login unavailable: desktop requires Qt WebEngineQuick; Android is disabled pending native pre-navigation enforcement.")
endif()

# Android's existing KDAB integration already supplies target-ABI OpenSSL.
if(NOT TARGET OpenSSL::Crypto)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    set_property(TARGET OpenSSL::Crypto PROPERTY IMPORTED_GLOBAL TRUE)
endif()

qt_add_library(rh_sessions STATIC
    securecredentiallocker.cpp securecredentiallocker.h
    rhcredentialstore.cpp rhcredentialstore.h
    rhsessionmanager.cpp rhsessionmanager_auth.cpp rhsessionmanager_accounts.cpp rhsessionmanager.h
)
target_include_directories(rh_sessions PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${QTKEYCHAIN_INCLUDE_DIRS}
    ${qtkeychain_SOURCE_DIR}
    ${qtkeychain_BINARY_DIR}
)
target_link_libraries(rh_sessions PUBLIC rh_network_engine rh_browser_auth ${PUBLICS_QTKEYCHAIN_TARGET} PRIVATE OpenSSL::Crypto)

qt_add_library(robin_hood_applet STATIC robinhoodapplet.cpp robinhoodapplet.h)
target_include_directories(robin_hood_applet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(robin_hood_applet PUBLIC applet_contract PRIVATE rh_sessions Qt6::Qml)
qt_add_resources(robin_hood_applet rh_config PREFIX "/robinhood" FILES config/robinhood-api.json)
add_subdirectory(qml/RobinHoodInterface)
