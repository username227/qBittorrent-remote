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

# SVG icons from the bundled qBittorrent UI require the QtSvg icon-engine
# plugin on every platform. Make the dependency explicit so missing SVG
# support fails at configure time rather than silently hiding all icons.
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Widgets Network Svg)

# Optional cross-platform OS credential vault: libsecret/Secret Service,
# Windows Credential Manager or macOS Keychain. Never allow plaintext fallback.
find_package(Qt6Keychain QUIET)
# QtKeychain exports 'qt6keychain' on Arch and most distributions.
# Some packages provide a namespaced target; support either without forcing
# any particular Linux keyring provider or system library.
if(Qt6Keychain_FOUND AND TARGET qt6keychain)
    set(QBT_REMOTE_KEYCHAIN_TARGET qt6keychain)
elseif(Qt6Keychain_FOUND AND TARGET Qt6Keychain::Qt6Keychain)
    set(QBT_REMOTE_KEYCHAIN_TARGET Qt6Keychain::Qt6Keychain)
endif()

option(QBT_REMOTE_REQUIRE_KEYCHAIN
    "Fail configuration if secure QtKeychain support is unavailable" OFF)
if(QBT_REMOTE_REQUIRE_KEYCHAIN AND NOT QBT_REMOTE_KEYCHAIN_TARGET)
    message(FATAL_ERROR
        "This build requires QtKeychain; install the Qt 6 QtKeychain development package.")
endif()

if(QBT_REMOTE_KEYCHAIN_TARGET)
    message(STATUS "QtKeychain found: secure Remember password available")
else()
    message(STATUS "Secure QtKeychain target unavailable: Remember password disabled")
endif()

add_library(qbittorrent_remote_core STATIC
    src/api/ServerAddress.cpp
    src/api/ServerAddress.h
    src/api/ServerProfile.cpp
    src/api/ServerProfile.h
    src/api/PathMappingStore.cpp
    src/api/PathMappingStore.h
    src/api/TorrentMetadataReader.cpp
    src/api/TorrentMetadataReader.h
    src/api/RemoteSession.cpp
    src/api/RemoteSession.h
    src/api/CredentialVault.cpp
    src/api/CredentialVault.h
)
target_include_directories(qbittorrent_remote_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(qbittorrent_remote_core PUBLIC Qt6::Core Qt6::Network)
if(QBT_REMOTE_KEYCHAIN_TARGET)
    target_link_libraries(qbittorrent_remote_core PUBLIC ${QBT_REMOTE_KEYCHAIN_TARGET})
    target_compile_definitions(qbittorrent_remote_core PRIVATE
        QBT_REMOTE_WITH_KEYCHAIN=1)
endif()

# Shared by the real application and CTest GUI workflows.
add_library(qbittorrent_remote_gui STATIC
    src/gui/MainWindow.cpp
    src/gui/MainWindow.h
    src/gui/ConnectionDialog.cpp
    src/gui/ConnectionDialog.h
    src/gui/RemotePreferencesDialog.cpp
    src/gui/RemotePreferencesDialog.h
    src/gui/RemoteAddTorrentDialog.cpp
    src/gui/RemoteAddTorrentDialog.h
    src/gui/RemoteFileSystemPathComboEdit.cpp
    src/gui/RemoteFileSystemPathComboEdit.h
    src/gui/RemoteTorrentContentWidget.cpp
    src/gui/RemoteTorrentContentWidget.h
    src/gui/addnewtorrentdialog.ui
    src/gui/optionsdialog.ui
    src/gui/FilterSidebar.cpp
    src/gui/FilterSidebar.h
    src/gui/TorrentModel.cpp
    src/gui/TorrentModel.h
    src/gui/TorrentDetailsWidget.cpp
    src/gui/TorrentDetailsWidget.h
    src/gui/remotepropertieswidget.ui
    src/gui/ServerPane.cpp
    src/gui/ServerPane.h
    src/gui/upstream/transferlistfilterswidgetitem.cpp
    src/gui/upstream/transferlistfilterswidgetitem.h
    src/gui/upstream/proptabbar.cpp
    src/gui/upstream/proptabbar.h
    src/gui/upstream/progressbarpainter.cpp
    src/gui/upstream/progressbarpainter.h
    src/gui/upstream/transferlistdelegate.cpp
    src/gui/upstream/transferlistdelegate.h
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
target_link_libraries(qbittorrent_remote_gui PUBLIC qbittorrent_remote_core Qt6::Widgets Qt6::Svg)

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

# Linux Wayland compositors resolve app icons from the desktop entry matched
# by QGuiApplication::desktopFileName(), not necessarily from setWindowIcon().
# Install both pieces for a correct native app_id and task-list icon. The
# same entry also gives X11 desktops a stable application identity.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(FILES packaging/linux/qbittorrent-remote.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES src/gui/icons/qbittorrent-tray.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        RENAME qbittorrent-remote.svg)
endif()

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

    add_executable(test_profile_workflow
        tests/test_profile_workflow.cpp
    )
    target_link_libraries(test_profile_workflow PRIVATE
        qbittorrent_remote_gui
        Qt6::Test
    )
    add_test(NAME server_profile_management COMMAND test_profile_workflow)

    add_executable(test_preferences_workflow
        tests/test_preferences_workflow.cpp
    )
    target_link_libraries(test_preferences_workflow PRIVATE
        qbittorrent_remote_gui
        Qt6::Test
    )
    add_test(NAME native_preferences_and_remote_paths COMMAND test_preferences_workflow)

    add_executable(test_add_torrent_workflow
        tests/test_add_torrent_workflow.cpp
    )
    target_link_libraries(test_add_torrent_workflow PRIVATE
        qbittorrent_remote_gui
        Qt6::Test
    )
    add_test(NAME native_add_torrent_and_path_mapping COMMAND test_add_torrent_workflow)
    # Unexpected modal dialogs must not stall CI indefinitely.
    set_tests_properties(native_add_torrent_and_path_mapping PROPERTIES TIMEOUT 75)

    # Native click-through coverage for Status, Categories, Tags and
    # Trackers; keep the Add Torrent workflows merged from PR #10.
    add_executable(test_sidebar_filter_workflow
        tests/test_sidebar_filter_workflow.cpp
    )
    target_link_libraries(test_sidebar_filter_workflow PRIVATE
        qbittorrent_remote_gui
        Qt6::Test
    )
    add_test(NAME combined_sidebar_filter_workflow COMMAND test_sidebar_filter_workflow)

    # Interactive Content, Peers and Trackers sorting must work in the actual
    # Qt detail widgets, including hierarchy and numeric raw-value ordering.
    add_executable(test_details_sorting_workflow
        tests/test_details_sorting_workflow.cpp
    )
    target_link_libraries(test_details_sorting_workflow PRIVATE
        qbittorrent_remote_gui
        Qt6::Test
    )
    add_test(NAME detail_tables_sorting_workflow COMMAND test_details_sorting_workflow)

    # Right-click actions and multi-selection on the native torrent list.
    # Copy operates only on data supplied by the selected remote server.
    add_executable(test_torrent_context_menu_workflow
        tests/test_torrent_context_menu_workflow.cpp
    )
    target_link_libraries(test_torrent_context_menu_workflow PRIVATE
        qbittorrent_remote_gui
        Qt6::Test
    )
    add_test(NAME torrent_context_menu_workflow COMMAND test_torrent_context_menu_workflow)

    add_executable(test_connection_workflow
        tests/test_connection_workflow.cpp
    )
    target_link_libraries(test_connection_workflow PRIVATE
        qbittorrent_remote_gui
        Qt6::Test
        Qt6::Network
    )
    add_test(NAME remote_connection_workflow COMMAND test_connection_workflow)
    # Modal file-picker/duplicate-confirmation GUI workflows must never stall CI.
    set_tests_properties(remote_connection_workflow PROPERTIES TIMEOUT 120)
    set_tests_properties(remote_connection_workflow PROPERTIES TIMEOUT 100)

    # Tests start local TLS endpoints with ephemeral certificates and drive
    # real certificate-decision GUI workflows; no production server involved.
    add_executable(test_tls_trust_workflow
        tests/test_tls_trust_workflow.cpp
    )
    target_link_libraries(test_tls_trust_workflow PRIVATE
        qbittorrent_remote_gui
        Qt6::Test
        Qt6::Network
    )
    add_test(NAME certificate_trust_workflow COMMAND test_tls_trust_workflow)
    # A stalled TLS handshake or modal UI must not hang the complete CTest run.
    set_tests_properties(certificate_trust_workflow PROPERTIES TIMEOUT 90)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_tests_properties(window_open_and_quit server_profile_management native_preferences_and_remote_paths native_add_torrent_and_path_mapping combined_sidebar_filter_workflow detail_tables_sorting_workflow torrent_context_menu_workflow remote_connection_workflow certificate_trust_workflow PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endif()
endif()
