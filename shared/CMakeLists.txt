qt_add_library(applet_contract STATIC
    appletinterface.cpp
    appletinterface.h
)
target_include_directories(applet_contract PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(applet_contract PUBLIC Qt6::Core)
