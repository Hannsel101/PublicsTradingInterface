# Robinhood network/trading policy remains separate from authentication UI.
qt_add_library(rh_network_engine STATIC
    rhnetworkengine.cpp
    rhnetworkengine.h
    rhoauthgrant.cpp rhoauthgrant.h
)
target_include_directories(rh_network_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rh_network_engine PUBLIC Qt6::Network)
publics_enable_live_trading(rh_network_engine)

# Retained only for legacy compatibility/regression coverage, not production login.
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
    target_compile_definitions(rh_browser_auth PRIVATE PUBLICS_RH_ANDROID_WEBVIEW=1)
    target_link_libraries(rh_browser_auth PRIVATE Qt6::WebView)
endif()

# Android's existing KDAB integration supplies target-ABI OpenSSL.
if(NOT TARGET OpenSSL::Crypto)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    set_property(TARGET OpenSSL::Crypto PROPERTY IMPORTED_GLOBAL TRUE)
endif()
qt_add_library(rh_secure_credentials STATIC
    securecredentiallocker.cpp securecredentiallocker.h
    rhcredentialstore.cpp rhcredentialstore.h
)
target_include_directories(rh_secure_credentials PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${QTKEYCHAIN_INCLUDE_DIRS}
    ${qtkeychain_SOURCE_DIR}
    ${qtkeychain_BINARY_DIR}
)
target_link_libraries(rh_secure_credentials PUBLIC Qt6::Core ${PUBLICS_QTKEYCHAIN_TARGET} PRIVATE OpenSSL::Crypto)

qt_add_library(rh_native_auth STATIC rhnativeauthclient.cpp rhnativeauthclient.h)
target_include_directories(rh_native_auth PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rh_native_auth PUBLIC rh_network_engine PRIVATE rh_secure_credentials)

find_package(Qt6 6.10 REQUIRED COMPONENTS Concurrent)
qt_add_library(rh_sessions STATIC
    rhsessionmanager.cpp rhsessionmanager_auth.cpp rhsessionmanager_accounts.cpp
    rhsessionmanager_trading.cpp rhsessionmanager_oauth.cpp rhsessionmanager_native.cpp rhsessionmanager.h
)
target_include_directories(rh_sessions PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rh_sessions PUBLIC rh_network_engine rh_browser_auth rh_native_auth rh_secure_credentials PRIVATE Qt6::Concurrent)

qt_add_library(rh_trading STATIC rhtradecontroller.cpp rhtradecontroller.h rhtradingcontext.cpp rhtradingcontext.h)
target_include_directories(rh_trading PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rh_trading PUBLIC rh_network_engine rh_sessions)

qt_add_library(robin_hood_applet STATIC robinhoodapplet.cpp robinhoodapplet.h)
target_include_directories(robin_hood_applet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(robin_hood_applet PUBLIC applet_contract PRIVATE rh_sessions rh_trading Qt6::Qml)
qt_add_resources(robin_hood_applet rh_config PREFIX "/robinhood"
    FILES config/robinhood-api.json third-party/robin-stocks-LICENSE.txt
)
add_subdirectory(qml/RobinHoodInterface)
