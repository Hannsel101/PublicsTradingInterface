cmake_minimum_required(VERSION 3.24)
project(RobinhoodNativeAuthSpike VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network Widgets)
qt_standard_project_setup(REQUIRES 6.11)
get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

qt_add_library(native_auth_core STATIC
    native_auth_client.cpp
    native_auth_client.h
    "${REPOSITORY_ROOT}/applets/robin_hood/rhoauthgrant.cpp"
    "${REPOSITORY_ROOT}/applets/robin_hood/rhoauthgrant.h"
)
target_include_directories(native_auth_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${REPOSITORY_ROOT}/applets/robin_hood"
)
target_link_libraries(native_auth_core PUBLIC Qt6::Core Qt6::Network)
qt_add_executable(robinhood_native_auth_spike MANUAL_FINALIZATION main.cpp)
target_link_libraries(robinhood_native_auth_spike PRIVATE native_auth_core Qt6::Widgets)
set_target_properties(robinhood_native_auth_spike PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.publicstradinginterface.robinhoodauthspike"
    MACOSX_BUNDLE_BUNDLE_NAME "Robinhood Native Auth Test"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
    WIN32_EXECUTABLE TRUE
)
if(ANDROID)
    include(FetchContent)
    FetchContent_Declare(android_openssl
        GIT_REPOSITORY https://github.com/KDAB/android_openssl.git
        GIT_TAG b71f1470962019bd89534a2919f5925f93bc5779
    )
    FetchContent_MakeAvailable(android_openssl)
    include("${android_openssl_SOURCE_DIR}/android_openssl.cmake")
    add_android_openssl_libraries(robinhood_native_auth_spike)
    set_target_properties(robinhood_native_auth_spike PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
        QT_ANDROID_APP_NAME "Robinhood Native Auth Test"
        QT_ANDROID_VERSION_CODE 1
        QT_ANDROID_VERSION_NAME "0.1.0"
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_TARGET_SDK_VERSION 36
    )
endif()
qt_finalize_executable(robinhood_native_auth_spike)
include(CTest)
if(BUILD_TESTING AND NOT ANDROID)
    find_package(Qt6 6.11 REQUIRED COMPONENTS Test)
    qt_add_executable(test_native_auth_client test_native_auth_client.cpp)
    target_link_libraries(test_native_auth_client PRIVATE native_auth_core Qt6::Test)
    add_test(NAME native_auth_client COMMAND test_native_auth_client)
    qt_add_executable(test_native_auth_ui test_native_auth_ui.cpp)
    target_link_libraries(test_native_auth_ui PRIVATE native_auth_core Qt6::Widgets Qt6::Test)
    add_test(NAME native_auth_ui COMMAND test_native_auth_ui)
    set_tests_properties(native_auth_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME native_auth_offline_smoke COMMAND robinhood_native_auth_spike --offline-smoke)
    set_tests_properties(native_auth_offline_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
