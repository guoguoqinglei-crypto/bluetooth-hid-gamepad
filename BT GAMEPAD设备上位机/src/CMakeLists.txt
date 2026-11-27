cmake_minimum_required(VERSION 3.16)

project(btgamepad LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(PROJECT_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    hiddevice.cpp
    hiddevice.h
    protocol.h
    dashboard.h
    performance_monitor.cpp
    performance_monitor.h
)

qt_add_executable(btgamepad WIN32 MANUAL_FINALIZATION ${PROJECT_SOURCES})

target_link_libraries(btgamepad PRIVATE
    Qt6::Widgets
    hid
    hidparse
    setupapi
)

set_target_properties(btgamepad PROPERTIES
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS btgamepad
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_finalize_executable(btgamepad)
