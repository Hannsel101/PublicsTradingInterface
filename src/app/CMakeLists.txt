qt_add_executable(appPublicsTradingInterface main.cpp)

if(APPLE AND NOT IOS)
    set(PUBLICS_MACOS_ICON "${PROJECT_SOURCE_DIR}/resources/PublicsTradingInterface.icns")
    set_source_files_properties("${PUBLICS_MACOS_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
    )
    target_sources(appPublicsTradingInterface PRIVATE "${PUBLICS_MACOS_ICON}")
    set_target_properties(appPublicsTradingInterface PROPERTIES
        MACOSX_BUNDLE_ICON_FILE PublicsTradingInterface.icns
    )
elseif(WIN32)
    target_sources(appPublicsTradingInterface PRIVATE
        "${PROJECT_SOURCE_DIR}/resources/PublicsTradingInterface.rc"
    )
endif()

if((APPLE AND NOT IOS) OR WIN32)
    qt_add_resources(appPublicsTradingInterface desktop_icon
        PREFIX "/"
        BASE "${PROJECT_SOURCE_DIR}"
        FILES "${PROJECT_SOURCE_DIR}/ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png"
    )
endif()

if(IOS)
    set(PUBLICS_IOS_ASSET_CATALOG
        "${PROJECT_SOURCE_DIR}/ios/Assets.xcassets"
    )
    target_sources(appPublicsTradingInterface PRIVATE
        "${PUBLICS_IOS_ASSET_CATALOG}"
    )
    set_source_files_properties("${PUBLICS_IOS_ASSET_CATALOG}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
    )
endif()

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
set_target_properties(appPublicsTradingInterface PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER trb.appPublicsTradingInterface
    MACOSX_BUNDLE_BUNDLE_VERSION ${PUBLICS_IOS_BUILD_NUMBER}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
)

target_link_libraries(appPublicsTradingInterface PRIVATE
    Qt6::Quick publics_auth publics_api publics_trading
    publics_ui publics_uiplugin
)
publics_enable_live_trading(appPublicsTradingInterface)

if(ANDROID)
    set_target_properties(appPublicsTradingInterface PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${PROJECT_SOURCE_DIR}/android"
        QT_ANDROID_PACKAGE_NAME "org.qtproject.example.appPublicsTradingInterface"
        QT_ANDROID_APP_NAME "Publics Trading Interface"
        QT_ANDROID_VERSION_NAME "2.1.2"
        QT_ANDROID_VERSION_CODE 4
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_TARGET_SDK_VERSION 36
    )
    add_android_openssl_libraries(appPublicsTradingInterface)
endif()

install(TARGETS appPublicsTradingInterface
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

