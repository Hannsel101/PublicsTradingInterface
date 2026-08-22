cmake_minimum_required(VERSION 3.16)
project(PublicsTradingInterface VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Quick Network Concurrent)

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
    resources.qrc
)

qt_add_qml_module(appPublicsTradingInterface
    URI PublicsTradingInterface
    VERSION 1.0

    SOURCES
        apiHandlers/publicauthclient.h apiHandlers/publicauthclient.cpp
        apiHandlers/publicapiworker.h apiHandlers/publicapiworker.cpp
        apiHandlers/stocksearchcontroller.h apiHandlers/stocksearchcontroller.cpp
    RESOURCES
        resources.qrc

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
)

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
