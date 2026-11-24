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
    src/gui/RemoteCategoryDialog.cpp
    src/gui/RemoteCategoryDialog.h
    src/gui/remotetorrentcategorydialog.ui
    src/gui/RemoteSpeedLimitsDialog.cpp
    src/gui/RemoteSpeedLimitsDialog.h
    src/gui/upstream/layouts/speedlimitdialog.ui
    src/gui/RemoteAddTorrentDialog.cpp
    src/gui/RemoteAddTorrentDialog.h
    src/gui/RemoteTrackerEntriesDialog.cpp
    src/gui/RemoteTrackerEntriesDialog.h
    src/gui/RemoteTorrentContentLayoutDialog.cpp
    src/gui/RemoteTorrentContentLayoutDialog.h
    src/gui/remotecontentlayoutdialog.ui
    src/gui/RemoteTorrentShareLimitsWidget.cpp
    src/gui/RemoteTorrentShareLimitsWidget.h
    src/gui/remotetorrentsharelimitswidget.ui
    src/gui/RemoteTorrentOptionsDialog.cpp
    src/gui/RemoteTorrentOptionsDialog.h
    src/gui/remotetorrentoptionsdialog.ui
    src/gui/trackerentriesdialog.ui
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
    src/gui/MappedContentTreeWidget.cpp
    src/gui/MappedContentTreeWidget.h
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
    AUTOUIC_SEARCH_PATHS "${PROJECT_SOURCE_DIR}/src/gui/upstream;${PROJECT_SOURCE_DIR}/src/gui/upstream/layouts"
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

if(BUILD_TESTING)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Test)

    # Tests exercise user-visible workflows, not implementation units
    # or hard-coded protocol inputs. All tests run through CTest.
    # One linked executable; each CTest entry starts its own process and
    # selects the original QtTest suite (and optional test methods).
    add_executable(qbittorrent_remote_tests
        tests/workflow_main.cpp
        tests/test_window_workflow.cpp
        tests/test_profile_workflow.cpp
        tests/test_preferences_workflow.cpp
        tests/test_add_torrent_workflow.cpp
        tests/test_sidebar_filter_workflow.cpp
        tests/test_details_sorting_workflow.cpp
        tests/test_torrent_context_menu_workflow.cpp
        tests/test_connection_workflow.cpp
        tests/test_tls_trust_workflow.cpp
    )
    target_link_libraries(qbittorrent_remote_tests PRIVATE
        qbittorrent_remote_gui
        Qt6::Test
        Qt6::Network
    )
    add_test(NAME window_open_and_quit COMMAND qbittorrent_remote_tests window)
    add_test(NAME server_profile_management COMMAND qbittorrent_remote_tests profile)
    add_test(NAME native_preferences_and_remote_paths COMMAND qbittorrent_remote_tests preferences)
    add_test(NAME native_add_torrent_and_path_mapping COMMAND qbittorrent_remote_tests add_torrent)
    set_tests_properties(native_add_torrent_and_path_mapping PROPERTIES TIMEOUT 75)
    add_test(NAME combined_sidebar_filter_workflow COMMAND qbittorrent_remote_tests sidebar_filter)
    add_test(NAME detail_tables_sorting_workflow COMMAND qbittorrent_remote_tests details_sorting)
    add_test(NAME torrent_context_menu_workflow COMMAND qbittorrent_remote_tests torrent_context_menu)

    # Keep the shared authenticated HTTP fixture and its independent
    # logical groups; a stalled dialog must not block other groups.
    set(remote_workflow_methods "")
    function(add_remote_workflow_group name timeout)
        foreach(method IN LISTS ARGN)
            if(method IN_LIST remote_workflow_methods)
                message(FATAL_ERROR "Remote workflow registered twice: ${method}")
            endif()
            list(APPEND remote_workflow_methods "${method}")
        endforeach()
        set(remote_workflow_methods "${remote_workflow_methods}" PARENT_SCOPE)
        add_test(NAME ${name} COMMAND qbittorrent_remote_tests connection ${ARGN})
        set_tests_properties(${name} PROPERTIES
            TIMEOUT ${timeout}
            LABELS "remote;functional"
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endfunction()

    add_remote_workflow_group(remote_authentication_workflow 60
        connectionDialogGeometryUsesSameQSettingsFile
        userCanConnectAndDisconnect
        userCanRememberAndReusePasswordWithoutSavingPlaintext
        removingSavedServerForgetsItsProtectedCredential
        userCanConnectWithBlankPasswordWhenServerPermitsIt
        startupConnectsOnlyOptedInProfilesUsingProtectedCredentials
        userSeesAuthenticationFailure
        modernServerLoginAndPreferencesAcceptNoContent
    )

    add_remote_workflow_group(remote_server_isolation_workflow 60
        mainWindowAndEachServerRestoreNativeLayout
        simultaneousServersHaveIndependentTabsAndKeepPolling
    )

    add_remote_workflow_group(remote_tray_and_status_workflow 60
        nativeTrayMenuControlsRemoteSessionAndSpeedLimitsWhileHidden
        nativeTrayMenuTracksTheSelectedServerWithoutCrossServerCommands
        toolbarAndStatusbarRemainEnabledAfterHiddenWindowExit
        corruptedOldToolbarSettingsRecoverOnce
        statusBarFieldsStayFixedDuringAuthenticatedRateUpdates
    )

    add_remote_workflow_group(remote_transfer_presentation_workflow 60
        globalFontSizeKeepsTorrentListAsFloorAndPersists
        liveTorrentRowsFollowFullUpdatesPatchesAndRemovals
        liveTorrentSwarmCountsHonorHideZeroAndIncrementalSync
        nativeTransferPresentationShowsUpstreamProgressAndAvailability
        upstreamTorrentStateColorsSurviveStalledAndQueuedStates
    )

    add_remote_workflow_group(remote_torrent_addition_workflow 75
        torrentFileUploadPreservesBinaryMetadata
        userDropsTorrentFilesAndMagnetIntoNativeTransferView
        userCanSelectSevenTorrentFilesAndUploadOneAuthenticatedBatch
        duplicateTorrentOffersTrackerMergeThroughNativeGui
        privateTorrentsNeverOfferOrSendTrackerMerge
        nativeAddTorrentOptionsUseAuthenticatedServerApi
    )

    add_remote_workflow_group(remote_content_workflow 75
        nativeTorrentDetailsUseSelectedRemoteServerAndFilePriorityApis
        contentDragPreferenceIsEnabledAndControlsNativeDragMode
        userDragsOnlyExistingMappedContentAsCopyUris
        userOpensMappedRemoteTorrentLocationInDesktopFileManager
        userSetsRemoteLocationUsingActualTorrentContextMenu
        nativeManageContentAndTorrentOptionsChangeServerState
        remoteTorrentExportReturnsActualMetadata
    )

    add_remote_workflow_group(remote_torrent_actions_workflow 75
        userQueueSubmenuSendsSelectedTorrentCommandsToAuthenticatedServer
        userForceActionsReachSelectedAuthenticatedRemoteServer
        rejectedTorrentActionDoesNotDisconnectServer
        userRenamesSelectedTorrentThroughActualContextMenuAndDialog
        realTorrentActionsUseAuthenticatedServerApis
        automaticTorrentManagementRequiresUserConsent
    )

    add_remote_workflow_group(remote_tracker_workflow 60
        nativeTrackerListEditorReplacesAllSelectedTorrents
        privateTrackerAdditionPolicyThroughEditor
    )

    add_remote_workflow_group(remote_sidebar_management_workflow 75
        nativeSidebarMenusManageSelectedServerAndVisibleTorrents
    )

    add_remote_workflow_group(remote_preferences_workflow 60
        remotePreferencesReadApplyVerifyAndCancelOnSelectedServer
        rejectedAndIgnoredRemotePreferenceWritesKeepEditsStaged
    )

    add_remote_workflow_group(remote_downloads_preferences_workflow 60
        downloadsSavingManagementUsesSelectedServerAndMappedPaths
        downloadsAdditionDefaultsFollowSelectedServer
        downloadsDialogBehaviorAndDuplicateDefaults
        downloadsBackupsAndWatchedFoldersUseSelectedServer
        downloadsExclusionsFollowSelectedServerCapabilities
        downloadsNotificationsAndProgramsFollowSelectedServer
        localOriginalTorrentDeletionRequiresConfirmedAddition
        legacyBackupDestinationsUseNativeControls
    )

    add_remote_workflow_group(remote_connection_preferences_workflow 60
        connectionProtocolAndListeningPortFollowSelectedServer
        connectionLimitsFollowSelectedServer
        i2pConfigurationFollowsSelectedServerAndBuild
        proxyConfigurationFollowsSelectedServer
    )

    # Check the registration against the actual QtTest-discovered functions.
    # Future additions (including remote Preferences) must be assigned to a
    # group rather than silently falling out of the CTest suite.
    add_test(NAME remote_workflow_registration
        COMMAND ${CMAKE_COMMAND}
            "-DTEST_EXECUTABLE=$<TARGET_FILE:qbittorrent_remote_tests>"
            "-DREGISTERED_METHODS=${remote_workflow_methods}"
            -P "${PROJECT_SOURCE_DIR}/tests/check_remote_workflow_registration.cmake"
    )
    set_tests_properties(remote_workflow_registration PROPERTIES
        TIMEOUT 20
        LABELS "remote;registration"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    # Tests start local TLS endpoints with ephemeral certificates and drive
    # real certificate-decision GUI workflows; no production server involved.
    add_test(NAME certificate_trust_workflow COMMAND qbittorrent_remote_tests tls_trust)
    # A stalled TLS handshake or modal UI must not hang the complete CTest run.
    set_tests_properties(certificate_trust_workflow PROPERTIES TIMEOUT 90)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_tests_properties(window_open_and_quit server_profile_management native_preferences_and_remote_paths native_add_torrent_and_path_mapping combined_sidebar_filter_workflow detail_tables_sorting_workflow torrent_context_menu_workflow certificate_trust_workflow PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endif()
endif()
