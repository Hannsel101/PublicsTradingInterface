function(publics_add_test target source)
    qt_add_executable(${target} ${source})
    set_target_properties(${target} PROPERTIES MACOSX_BUNDLE FALSE WIN32_EXECUTABLE FALSE)
    target_link_libraries(${target} PRIVATE Qt6::Test Qt6::Gui ${ARGN})
    add_test(NAME ${target}
        COMMAND ${CMAKE_COMMAND} -E env --unset=PUBLICS_RUN_KEYCHAIN_TEST
            $<TARGET_FILE:${target}>
    )
    set_tests_properties(${target} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        TIMEOUT 120
    )
endfunction()

publics_add_test(test_publicauthclient integration/test_publicauthclient.cpp publics_auth)
publics_add_test(test_publicapiworker unit/test_publicapiworker.cpp publics_api)
publics_add_test(test_launcher unit/test_launcher.cpp launcher_controller)
if(TARGET telegram_bots_ui)
    publics_add_test(test_telegramdebuglogging unit/test_telegramdebuglogging.cpp telegram_bots_runtime)
    target_compile_definitions(test_telegramdebuglogging PRIVATE
        "PUBLICS_EXPECT_INCOMING_LOGS=$<CONFIG:Debug>")
    publics_add_test(test_telegrampublicstrades integration/test_telegrampublicstrades.cpp
        telegram_bots_trading publics_trading)
    target_compile_definitions(test_telegrampublicstrades PRIVATE
        "PUBLICS_TELEGRAM_EXPECT_LIVE=$<OR:$<CONFIG:Release>,$<BOOL:${RELEASE_BUILD}>>")
    publics_add_test(test_telegramtradebridge integration/test_telegramtradebridge.cpp
        telegram_bots_applet telegram_bots_backend telegram_bots_trading publics_auth publics_api publics_trading)
    set_tests_properties(test_telegramtradebridge PROPERTIES TIMEOUT 120)
    publics_add_test(test_telegrambotstore unit/test_telegrambotstore.cpp telegram_bots_storage OpenSSL::Crypto)
    publics_add_test(test_telegrambotruntime unit/test_telegrambotruntime.cpp telegram_bots_runtime)
    publics_add_test(test_telegramwebhookhost unit/test_telegramwebhookhost.cpp telegram_bots_runtime)
    publics_add_test(test_telegramwebhookmanager unit/test_telegramwebhookmanager.cpp telegram_bots_runtime)
    publics_add_test(test_telegramwebhookagent unit/test_telegramwebhookagent.cpp telegram_bots_runtime)
    set_tests_properties(test_telegrambotruntime test_telegramwebhookhost test_telegramwebhookagent PROPERTIES TIMEOUT 240)
    publics_add_test(test_telegrampasswordbridge integration/test_telegrampasswordbridge.cpp
        telegram_bots_backend Qt6::Network)
    add_dependencies(test_telegrampasswordbridge test_telegramwebhookagent)
    target_compile_definitions(test_telegrampasswordbridge PRIVATE
        "PUBLICS_WEBHOOK_TEST_EXECUTABLE=\"$<TARGET_FILE:test_telegramwebhookagent>\"")
    set_tests_properties(test_telegrampasswordbridge PROPERTIES TIMEOUT 240)
    publics_add_test(test_telegrambotscontroller unit/test_telegrambotscontroller.cpp
        telegram_bots_backend Qt6::Network
    )
    add_dependencies(test_telegrambotscontroller test_telegramwebhookagent)
    target_compile_definitions(test_telegrambotscontroller PRIVATE
        "PUBLICS_WEBHOOK_TEST_EXECUTABLE=\"$<TARGET_FILE:test_telegramwebhookagent>\""
    )
    publics_add_test(test_telegrambotsapplet unit/test_telegrambotsapplet.cpp
        telegram_bots_applet telegram_bots_backend launcher_controller telegram_bots_ui telegram_bots_uiplugin Qt6::Quick
    )
endif()
publics_add_test(test_publicsapplet unit/test_publicsapplet.cpp publics_applet Qt6::Qml)
publics_add_test(test_rhoauthgrant unit/test_rhoauthgrant.cpp rh_network_engine)
publics_add_test(test_rhnativeauthclient unit/test_rhnativeauthclient.cpp rh_native_auth)
publics_add_test(test_rhcredentialstore unit/test_rhcredentialstore.cpp rh_secure_credentials)
publics_add_test(test_rhsessionmanager_native unit/test_rhsessionmanager_native.cpp rh_sessions Qt6::Qml)
publics_add_test(test_rhnetworkengine unit/test_rhnetworkengine.cpp rh_network_engine)
publics_enable_live_trading(test_rhnetworkengine)
publics_add_test(test_rhtradecontroller unit/test_rhtradecontroller.cpp rh_trading)
# Data-driven deadline/reconciliation suite needs headroom under parallel kits.
set_tests_properties(test_rhtradecontroller PROPERTIES TIMEOUT 240)
publics_add_test(test_rhtradingcontext integration/test_rhtradingcontext.cpp rh_trading)
publics_add_test(test_rhbrowserlogin unit/test_rhbrowserlogin.cpp rh_browser_auth)
get_target_property(rh_test_browser_backend rh_browser_auth PUBLICS_RH_BROWSER_BACKEND)
if(rh_test_browser_backend STREQUAL "webengine")
    find_package(Qt6 6.10 REQUIRED COMPONENTS WebEngineQuick)
    publics_add_test(test_rhbrowser_native integration/test_rhbrowser_native.cpp
        rh_browser_auth Qt6::WebEngineQuick Qt6::Qml
    )
    set_tests_properties(test_rhbrowser_native PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_CHROMIUM_FLAGS=--disable-background-networking --disable-gpu"
        TIMEOUT 60
    )
endif()
publics_add_test(test_rhsessionmanager unit/test_rhsessionmanager.cpp rh_sessions)
publics_add_test(test_rhdevicetoken unit/test_rhdevicetoken.cpp rh_sessions)
publics_add_test(test_securecredentiallocker unit/test_securecredentiallocker.cpp rh_sessions OpenSSL::Crypto)
publics_add_test(test_robinhoodapplet unit/test_robinhoodapplet.cpp robin_hood_applet Qt6::Qml)
# Avoid QFINDTESTDATA selecting a sibling build's generated static-module QML.
foreach(native_form_test IN ITEMS test_rhsessionmanager_native test_robinhoodapplet)
    target_compile_definitions(${native_form_test} PRIVATE
        "PUBLICS_RH_NATIVE_FORM_SOURCE=\"${PROJECT_SOURCE_DIR}/applets/robin_hood/qml/RobinHoodInterface/NativeLoginForm.qml\""
    )
endforeach()
target_compile_definitions(test_robinhoodapplet PRIVATE
    "PUBLICS_RH_EXPECT_LIVE=$<OR:$<CONFIG:Release>,$<BOOL:${RELEASE_BUILD}>>"
)
publics_add_test(test_autotradecontroller integration/test_auto_sequence.cpp
    publics_trading_preflight publics_auth publics_api
)
if(ANDROID)
    add_android_openssl_libraries(test_publicapiworker)
endif()

qt_add_executable(test_qml qml/main.cpp)
set_target_properties(test_qml PROPERTIES MACOSX_BUNDLE FALSE WIN32_EXECUTABLE FALSE)
target_link_libraries(test_qml PRIVATE Qt6::QuickTest Qt6::Quick
    publics_ui publics_uiplugin rh_ui rh_uiplugin launcher_ui launcher_uiplugin
)
if(TARGET telegram_bots_ui)
    target_link_libraries(test_qml PRIVATE telegram_bots_ui telegram_bots_uiplugin)
    target_compile_definitions(test_qml PRIVATE PUBLICS_HAS_TELEGRAM_BOTS)
endif()
add_test(NAME test_qml COMMAND test_qml -input "${CMAKE_CURRENT_SOURCE_DIR}/qml")
set_tests_properties(test_qml PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    TIMEOUT 120
)
