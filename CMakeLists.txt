cmake_minimum_required(VERSION 3.16)
project(PublicsTradingInterface VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick Gui Network Concurrent Test)
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

qt_standard_project_setup(REQUIRES 6.10)

# Content of downloaded design from Figma to Qt or similar
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/importedcontent/CMakeLists.txt")
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/importedcontent)
endif()

set(API_HANDLERS
    apiHandlers/accountfetcher.cpp
    apiHandlers/accountfetcher.h
)

qt_add_executable(appPublicsTradingInterface
    main.cpp
    ${API_HANDLERS}
    vcpkg.json
)

qt_add_qml_module(appPublicsTradingInterface
    URI PublicsTradingInterface
    VERSION 1.0

    SOURCES
        apiHandlers/publicauthclient.h apiHandlers/publicauthclient.cpp
        apiHandlers/publicapiworker.h apiHandlers/publicapiworker.cpp
        apiHandlers/stocksearchcontroller.h apiHandlers/stocksearchcontroller.cpp
    QML_FILES
        Main.qml
        qml/DisplayAccountInformation/ApiKeyList.qml
        qml/DisplayAccountInformation/ChooseAccountButton.qml
        qml/DisplayAccountInformation/ValidityTimer.qml
        qml/PerformTrades/StockTrade.qml
        qml/PerformTrades/TradeResultScreen.qml
        qml/StockSearchAutoComplete/StockSearchAutoComplete.qml

)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
set_target_properties(appPublicsTradingInterface PROPERTIES
#    MACOSX_BUNDLE_GUI_IDENTIFIER com.example.appPublicsTradingInterface
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_include_directories(appPublicsTradingInterface PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/apiHandlers
    ${QTKEYCHAIN_INCLUDE_DIRS}
    ${qtkeychain_SOURCE_DIR}
    ${qtkeychain_BINARY_DIR}
)

target_link_libraries(appPublicsTradingInterface
    PRIVATE
        Qt6::Quick
        Qt6::Network
        Qt6::Concurrent
        ${PUBLICS_QTKEYCHAIN_TARGET}
)

if(ANDROID)
    add_android_openssl_libraries(appPublicsTradingInterface)
endif()

# When in release mode real trades will occur. Where in debug mode only
# preflight trades occur.
if(RELEASE_BUILD)
    add_compile_definitions(RELEASE)
endif()

install(TARGETS appPublicsTradingInterface
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(test_publicauthclient
        tests/test_publicauthclient.cpp
        apiHandlers/publicauthclient.h
        apiHandlers/publicauthclient.cpp
    )

    target_include_directories(test_publicauthclient PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/apiHandlers
        ${QTKEYCHAIN_INCLUDE_DIRS}
        ${qtkeychain_SOURCE_DIR}
        ${qtkeychain_BINARY_DIR}
    )

    target_link_libraries(test_publicauthclient
        PRIVATE
            Qt6::Test
            Qt6::Gui
            Qt6::Network
            Qt6::Concurrent
            ${PUBLICS_QTKEYCHAIN_TARGET}
    )

    add_test(NAME test_publicauthclient COMMAND test_publicauthclient)

    qt_add_executable(test_publicapiworker
        tests/test_publicapiworker.cpp
        apiHandlers/publicapiworker.h
        apiHandlers/publicapiworker.cpp
    )

    target_include_directories(test_publicapiworker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/apiHandlers
    )

    target_link_libraries(test_publicapiworker
        PRIVATE
            Qt6::Test
            Qt6::Network
            Qt6::Concurrent
    )

    if(ANDROID)
        add_android_openssl_libraries(test_publicapiworker)
    endif()

    add_test(NAME test_publicapiworker COMMAND test_publicapiworker)
endif()
