cmake_minimum_required(VERSION 3.21)

project(qBittorrentRemote VERSION 0.1.0 LANGUAGES CXX)

# CMake is the sole build definition; CTest is the sole test runner.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

include(GNUInstallDirs)
include(CTest)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Widgets)

add_library(qbittorrent_remote_core STATIC
    src/api/ServerAddress.cpp
    src/api/ServerAddress.h
)
target_include_directories(qbittorrent_remote_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(qbittorrent_remote_core PUBLIC Qt6::Core)

# Shared by the real application and CTest GUI workflows.
add_library(qbittorrent_remote_gui STATIC
    src/gui/MainWindow.cpp
    src/gui/MainWindow.h
    src/gui/FilterSidebar.cpp
    src/gui/FilterSidebar.h
    src/gui/upstream/transferlistfilterswidgetitem.cpp
    src/gui/upstream/transferlistfilterswidgetitem.h
    src/gui/upstream/proptabbar.cpp
    src/gui/upstream/proptabbar.h
    src/gui/upstream/hidabletabwidget.cpp
    src/gui/upstream/hidabletabwidget.h
    src/gui/upstream/mainwindow.ui
    src/gui/upstream/qbittorrent_icons.qrc
)
target_include_directories(qbittorrent_remote_gui PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
set_target_properties(qbittorrent_remote_gui PROPERTIES
    AUTOUIC_SEARCH_PATHS "${PROJECT_SOURCE_DIR}/src/gui/upstream"
)
target_link_libraries(qbittorrent_remote_gui PUBLIC Qt6::Widgets)

qt_add_executable(qbittorrent-remote
    src/main.cpp
)
target_link_libraries(qbittorrent-remote PRIVATE
    qbittorrent_remote_core
    qbittorrent_remote_gui
)
set_target_properties(qbittorrent-remote PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

install(TARGETS qbittorrent-remote
    BUNDLE DESTINATION .
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

if(BUILD_TESTING)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Test)

    # Tests exercise user-visible workflows, not implementation units
    # or hard-coded protocol inputs. All tests run through CTest.
    add_executable(test_window_workflow
        tests/test_window_workflow.cpp
    )
    target_link_libraries(test_window_workflow PRIVATE
        qbittorrent_remote_gui
        Qt6::Test
    )

    add_test(NAME window_open_and_quit COMMAND test_window_workflow)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_tests_properties(window_open_and_quit PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endif()
endif()
