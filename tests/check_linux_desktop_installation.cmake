# SPDX-License-Identifier: GPL-3.0-or-later
# Exercise CMake's actual staged install, never a host installation.
set(stage "${BUILD_DIR}/desktop-install-test")
file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY "${stage}")
set(config_args)
if(NOT BUILD_CONFIG STREQUAL "")
    list(APPEND config_args --config "${BUILD_CONFIG}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${stage}"
    "${CMAKE_COMMAND}" --install "${BUILD_DIR}" ${config_args}
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Staged install failed: ${result}")
endif()
set(data "${stage}${INSTALL_DATADIR}")
set(bin "${stage}${INSTALL_BINDIR}")
set(desktop "${data}/applications/qbittorrent-remote.desktop")
foreach(path IN ITEMS "${desktop}"
        "${data}/icons/hicolor/scalable/apps/qbittorrent-remote.svg"
        "${bin}/qbittorrent-remote")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing installed desktop artifact: ${path}")
    endif()
endforeach()

# These utilities are required by Linux CI but optional on developer hosts.
# Do not update the user's desktop cache or default applications.
find_program(validator desktop-file-validate)
find_program(database update-desktop-database)
find_program(gio gio)
if(validator)
    execute_process(COMMAND "${validator}" "${desktop}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Installed desktop entry failed validation")
    endif()
endif()
if(database AND gio)
    execute_process(COMMAND "${database}" "${data}/applications"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Unable to register staged MIME handlers")
    endif()
    file(MAKE_DIRECTORY "${stage}/user-data" "${stage}/user-config")
    foreach(mime IN ITEMS application/x-bittorrent x-scheme-handler/magnet)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env
            "XDG_DATA_HOME=${stage}/user-data"
            "XDG_CONFIG_HOME=${stage}/user-config"
            "XDG_DATA_DIRS=${data}"
            "XDG_CONFIG_DIRS=${stage}/user-config"
            "PATH=${bin}:$ENV{PATH}"
            "LC_ALL=C"
            "${gio}" mime "${mime}"
            RESULT_VARIABLE result OUTPUT_VARIABLE handlers ERROR_VARIABLE errors)
        if(NOT result EQUAL 0 OR NOT handlers MATCHES "qbittorrent-remote\\.desktop")
            message(FATAL_ERROR "GIO cannot discover the installed ${mime} handler: ${handlers} ${errors}")
        endif()
    endforeach()
endif()
file(REMOVE_RECURSE "${stage}")
