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
publics_add_test(test_publicsapplet unit/test_publicsapplet.cpp publics_applet Qt6::Qml)
publics_add_test(test_rhnetworkengine unit/test_rhnetworkengine.cpp rh_network_engine)
publics_add_test(test_rhsessionmanager unit/test_rhsessionmanager.cpp rh_sessions)
publics_add_test(test_robinhoodapplet unit/test_robinhoodapplet.cpp robin_hood_applet Qt6::Qml)
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
add_test(NAME test_qml COMMAND test_qml -input "${CMAKE_CURRENT_SOURCE_DIR}/qml")
set_tests_properties(test_qml PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    TIMEOUT 120
)
