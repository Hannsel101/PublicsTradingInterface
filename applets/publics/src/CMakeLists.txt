qt_add_library(publics_auth STATIC
    auth/publicauthclient.cpp
    auth/publicauthclient.h
)
target_include_directories(publics_auth PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${QTKEYCHAIN_INCLUDE_DIRS}
    ${qtkeychain_SOURCE_DIR}
    ${qtkeychain_BINARY_DIR}
)
target_link_libraries(publics_auth PUBLIC
    Qt6::Core Qt6::Network ${PUBLICS_QTKEYCHAIN_TARGET}
)

qt_add_library(publics_api STATIC
    api/publicapiworker.cpp
    api/publicapiworker.h
)
target_include_directories(publics_api PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(publics_api PUBLIC Qt6::Core Qt6::Network Qt6::Concurrent)

function(publics_add_trading_target target)
    qt_add_library(${target} STATIC
        trading/autotradecontroller.cpp
        trading/autotradecontroller.h
    )
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${target} PUBLIC Qt6::Core PRIVATE publics_auth publics_api)
endfunction()

publics_add_trading_target(publics_trading)
publics_enable_live_trading(publics_trading)

qt_add_library(publics_applet STATIC
    publicsapplet.cpp
    publicsapplet.h
)
target_include_directories(publics_applet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(publics_applet
    PUBLIC applet_contract
    PRIVATE Qt6::Qml publics_auth publics_api publics_trading
)

if(BUILD_TESTING)
    # A separate compilation keeps Auto Mode tests preflight-only even in Release.
    publics_add_trading_target(publics_trading_preflight)
endif()
