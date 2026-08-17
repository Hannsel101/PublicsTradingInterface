cmake_minimum_required(VERSION 3.16)

project(PublicsTradingInterface VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick Network Concurrent)

# Find Qt6 Keychain provided by vcpkg
#find_package(KDABQtKeychain REQUIRED)
#find_package(Qt6Keychain REQUIRED)

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
    RESOURCE_PREFIX "/qt/qml"

    QML_FILES
        Main.qml
        qml/DisplayAccountInformation/ValidityTimer.qml
    SOURCES
        apiHandlers/publicauthclient.h apiHandlers/publicauthclient.cpp
        apiHandlers/publicapiworker.h apiHandlers/publicapiworker.cpp
        SOURCES apiHandlers/stocksearchcontroller.h apiHandlers/stocksearchcontroller.cpp
        QML_FILES qml/StockSearchAutoComplete/StockSearchAutoComplete.qml
        QML_FILES qml/PerformTrades/StockTrade.qml

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
)

target_link_libraries(appPublicsTradingInterface
    PRIVATE
        Qt6::Quick
        Qt6::Network
        Qt6::Concurrent
        Advapi32
        #KDABQtKeychain::qt6keychain
)

install(TARGETS appPublicsTradingInterface
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
