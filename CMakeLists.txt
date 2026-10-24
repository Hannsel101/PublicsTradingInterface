cmake_minimum_required(VERSION 3.21)
project(PublicsTradingInterface VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(PUBLICS_IOS_BUILD_NUMBER "2" CACHE STRING "iOS bundle build number")

find_package(Qt6 6.10 REQUIRED COMPONENTS Quick Gui Network Concurrent)
qt_standard_project_setup(REQUIRES 6.10)
include(CTest)
option(RELEASE_BUILD "Enable live orders for a custom build configuration" OFF)
find_package(Qt6Keychain CONFIG QUIET)

if(NOT Qt6Keychain_FOUND)
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build QtKeychain as a static library" FORCE)
    set(BUILD_WITH_QT6 ON CACHE BOOL "Build QtKeychain with Qt 6" FORCE)
    set(BUILD_TEST_APPLICATION OFF CACHE BOOL "Disable QtKeychain test app" FORCE)
    set(BUILD_TRANSLATIONS OFF CACHE BOOL "Disable QtKeychain translations" FORCE)
    FetchContent_Declare(
        qtkeychain
        GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain.git
        GIT_TAG 38ff50766c7e4ec490fecb2d9939b7cdc4b74b8e
    )
    FetchContent_MakeAvailable(qtkeychain)
endif()

if(ANDROID)
    include(FetchContent)
    FetchContent_Declare(
        android_openssl
        GIT_REPOSITORY https://github.com/KDAB/android_openssl.git
        GIT_TAG b71f1470962019bd89534a2919f5925f93bc5779
    )
    FetchContent_MakeAvailable(android_openssl)
    include(${android_openssl_SOURCE_DIR}/android_openssl.cmake)
endif()

set(PUBLICS_QTKEYCHAIN_TARGET "")
if(TARGET Qt6Keychain::Qt6Keychain)
    set(PUBLICS_QTKEYCHAIN_TARGET Qt6Keychain::Qt6Keychain)
elseif(TARGET qt6keychain)
    set(PUBLICS_QTKEYCHAIN_TARGET qt6keychain)
else()
    message(FATAL_ERROR "QtKeychain was found or fetched, but no Qt 6 target was exported")
endif()

# Only the app and its trading orchestration receive the live-order definition.
function(publics_enable_live_trading target)
    if(RELEASE_BUILD)
        target_compile_definitions(${target} PRIVATE RELEASE)
    else()
        target_compile_definitions(${target} PRIVATE $<$<CONFIG:Release>:RELEASE>)
    endif()
endfunction()

add_subdirectory(src)
add_subdirectory(qml/PublicsTradingInterface)
add_subdirectory(src/app)

if(BUILD_TESTING)
    find_package(Qt6 6.10 REQUIRED COMPONENTS Test QuickTest)
    add_subdirectory(tests)
endif()
