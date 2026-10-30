qt_add_library(telegram_bots_storage STATIC src/telegrambotstore.cpp src/telegrambotstore.h)
target_include_directories(telegram_bots_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE ${QTKEYCHAIN_INCLUDE_DIRS} ${qtkeychain_SOURCE_DIR} ${qtkeychain_BINARY_DIR}
)
target_link_libraries(telegram_bots_storage PUBLIC Qt6::Core
    PRIVATE Qt6::Concurrent OpenSSL::Crypto ${PUBLICS_QTKEYCHAIN_TARGET}
)

qt_add_library(telegram_bots_trading STATIC src/telegrampublicstradeservice.cpp src/telegrampublicstradeservice.h)
target_include_directories(telegram_bots_trading PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(telegram_bots_trading PUBLIC Qt6::Core publics_auth publics_api PRIVATE publics_trading)

qt_add_library(telegram_bots_applet STATIC src/telegrambotsapplet.cpp src/telegrambotsapplet.h)
find_package(Qt6 6.12 REQUIRED COMPONENTS HttpServer GLOBAL)
qt_add_library(telegram_bots_runtime STATIC src/telegramagent.cpp src/telegramagent.h
    src/telegrambotruntime.cpp src/telegrambotruntime.h
    src/telegramdebuglogging.cpp src/telegramdebuglogging.h
    src/telegraminspectorprocess.cpp src/telegraminspectorprocess.h
    src/telegramwebhookhost.cpp src/telegramwebhookhost.h
    src/telegramwebhookmanager.cpp src/telegramwebhookmanager.h)
target_include_directories(telegram_bots_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(telegram_bots_runtime PRIVATE "PUBLICS_TELEGRAM_DEBUG_PAYLOADS=$<CONFIG:Debug>")
target_link_libraries(telegram_bots_runtime PUBLIC Qt6::Network PRIVATE Qt6::HttpServer)
if(WIN32)
    target_link_libraries(telegram_bots_runtime PRIVATE iphlpapi ws2_32 advapi32 shell32 user32)
endif()
qt_add_library(telegram_bots_backend STATIC src/telegrambotscontroller.cpp src/telegrambotscontroller.h)
target_include_directories(telegram_bots_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(telegram_bots_backend PUBLIC telegram_bots_storage telegram_bots_runtime)
target_include_directories(telegram_bots_applet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(telegram_bots_applet PUBLIC applet_contract PRIVATE telegram_bots_backend telegram_bots_trading Qt6::Qml)

add_subdirectory(qml/TelegramBotsInterface)
