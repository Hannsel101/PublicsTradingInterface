qt_add_library(telegram_bots_storage STATIC src/telegrambotstore.cpp src/telegrambotstore.h)
target_include_directories(telegram_bots_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE ${QTKEYCHAIN_INCLUDE_DIRS} ${qtkeychain_SOURCE_DIR} ${qtkeychain_BINARY_DIR}
)
target_link_libraries(telegram_bots_storage PUBLIC Qt6::Core
    PRIVATE OpenSSL::Crypto ${PUBLICS_QTKEYCHAIN_TARGET}
)

qt_add_library(telegram_bots_applet STATIC src/telegrambotsapplet.cpp src/telegrambotsapplet.h)
qt_add_library(telegram_bots_runtime STATIC src/telegrambotruntime.cpp src/telegrambotruntime.h)
target_include_directories(telegram_bots_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(telegram_bots_runtime PUBLIC Qt6::Network)
qt_add_library(telegram_bots_backend STATIC src/telegrambotscontroller.cpp src/telegrambotscontroller.h)
target_include_directories(telegram_bots_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(telegram_bots_backend PUBLIC telegram_bots_storage telegram_bots_runtime)
target_include_directories(telegram_bots_applet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(telegram_bots_applet PUBLIC applet_contract PRIVATE telegram_bots_backend Qt6::Qml)

add_subdirectory(qml/TelegramBotsInterface)
